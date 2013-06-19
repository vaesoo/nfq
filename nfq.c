/*
 * nfq.c
 *
 * Holger Eitzenberger <holger@eitzenberger.org>, 2008, 2013.
 */
#define DEBUG
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>				/* for CPU_ZERO() */
#include <getopt.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <netlink/netlink.h>
#include <netlink/msg.h>
#include <netlink/object.h>
#include <netlink/netfilter/ct.h>
#include <netlink/netfilter/nfnl.h>
#include <netlink/netfilter/queue.h>
#include <netlink/netfilter/queue_msg.h>
#include <linux/netfilter.h>
#include <net/if.h>

#include "nfq.h"
#include "list.h"
#include "event.h"

#define DEFAULT_QUEUE_LEN		2048

#ifndef SOL_NETLINK
#define SOL_NETLINK 270
#endif

struct nfq_handler_stats {
	pthread_mutex_t nfqs_lock;
	uint64_t nfqs_bytes;
	uint64_t nfqs_pkts;
	uint64_t nfqs_overrun;
	uint64_t nfqs_err;
};

struct nfq_handler {
	struct list_head link;
	struct nl_sock *nfqh;
	struct nfnl_queue *nf_queue;
	int queueno;
	int queue_len;
	unsigned queue_batch;
	uint32_t queue_last_send_id;
	int cpu;
	struct nfq_handler_stats stats;
	union {
		struct nfq_thread {
			pthread_t tid;
		} thr;
	};
};

MY_LIST_HEAD(nfq_handler_list);

static long first_queueno, last_queueno;
static unsigned g_queue_len = DEFAULT_QUEUE_LEN;
static unsigned g_queue_batch = 1;
static int family = AF_INET;
static struct option lopts[] = {
	[0] = { "queue-len", required_argument, NULL, 0 },
	[1] = { "batch", required_argument, NULL, 0 },
    { "affinity", required_argument, NULL, 'a' },
    { "debug", no_argument, NULL, 'd' },
    { "forked", no_argument, NULL, 'f' },
	{ "help", no_argument, NULL, 'h' },
    { "queue", required_argument, NULL, 'q' },
    { "verbose", no_argument, NULL, 'v' },
    { 0 },
};
static bool is_forked;
static bool do_debug;
static int affinity_first_cpu = -1;
int verbose;

static struct {
	int family;
	char *str;
} family2str[] = {
	{ AF_INET, "inet" },
	{ AF_INET6, "inet6" },
};


const char *
f2s(int family)
{
	int i;

	for (i = 0; i < 2; i++) {
		if (family2str[i].family == family)
			return family2str[i].str;
	}

	return NULL;
}

static int
set_sockbuf_len(int fd, int len)
{
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &len, sizeof(len)) < 0) {
        xlog("setsockbuf: %m");
        return -1;
    }

    return 0;
}

static inline bool
with_affinity(void)
{
	return affinity_first_cpu >= 0;
}

static int
check_opts(int argc, char *argv[])
{
	long num_cpus, num_queues;
	char *end;

    do {
		int idx, c;

		if ((c = getopt_long(argc, argv, "a:dfhq:v", lopts, &idx)) == EOF)
			break;
        else if (c == 0) {
			switch (idx) {
			case 0:
				g_queue_len = strtoul(optarg, &end, 10);
				if (*end)
					die("queue-len: %s: garbage in input", optarg);
				break;

			case 1:
				g_queue_batch = strtoul(optarg, &end, 10);
				if (*end)
					die("batch: %s: garbage in input", optarg);
			}

			continue;
        }

        /* handle short opts */
        switch (c) {
		case 'a':
			affinity_first_cpu = strtoul(optarg, &end, 0);
			if (*end)
				die("%s: not a valid CPU number", optarg);
			break;

		case 'd':
			do_debug = true;
			break;

		case 'f':
			is_forked = true;
			break;

        case 'h':
			printf("usage: nfq --queue NO [--help]\n");
			exit(0);

        case 'q':
			first_queueno = last_queueno = strtol(optarg, &end, 0);
			if (*end == ':')
				last_queueno = strtol(end + 1, NULL, 0);
			if (first_queueno > last_queueno)
				die("%s: not a valid queue range", optarg);
			break;

		case 'v':
			verbose++;
			break;

        default:
            break;
        }
    } while (1);

	num_cpus = sysconf(_SC_NPROCESSORS_ONLN);
	num_queues = last_queueno - first_queueno + 1;

	if (num_queues > num_cpus)
		die("using more queues than CPUs is not supported");
	if (with_affinity() && affinity_first_cpu + num_queues > num_cpus)
		die("the first affinity CPU is too high");

    return 0;
}

static void
nfq_recv_valid_cb(struct nl_object *obj, void *arg)
{
	struct nfq_handler *qh = arg;
	struct nfnl_queue_msg *msg = (struct nfnl_queue_msg *)obj;
	const char *buf __UNUSED;
	int ret, len;

	buf = nfnl_queue_msg_get_payload(msg, &len);

	pthread_mutex_lock(&qh->stats.nfqs_lock);
	qh->stats.nfqs_pkts++;
	qh->stats.nfqs_bytes += len;
	pthread_mutex_unlock(&qh->stats.nfqs_lock);

	if (verbose > 2) {
		struct nl_dump_params dp = {
			.dp_type = NL_DUMP_STATS,
			.dp_fd = stdout,
			.dp_dump_msgtype = 1,
		};

		/* FIXME this requires rtnl/link cache to be filled */
		nl_object_dump(obj, &dp);
	}

	if (qh->queue_batch > 1) {
		const uint32_t id = nfnl_queue_msg_get_packetid(msg);
		
		if (id >= qh->queue_last_send_id + qh->queue_batch) {
			nfnl_queue_msg_set_verdict(msg, NF_ACCEPT);

			qh->queue_last_send_id = id;
			if ((ret = nfnl_queue_msg_send_verdict_batch(qh->nfqh, msg)) < 0)
				xlog("nfnl_queue_msg_send_verdict: %s", nl_geterror(ret));
		}
	} else {
		nfnl_queue_msg_set_verdict(msg, NF_ACCEPT);

		if ((ret = nfnl_queue_msg_send_verdict(qh->nfqh, msg)) < 0)
			xlog("nfnl_queue_msg_send_verdict: %s", nl_geterror(ret));
	}
}
 
static int
nfq_event_cb(struct nl_msg *msg, void *arg)
{
	struct nfq_handler *qh = arg;
	int ret;

	if ((ret = nl_msg_parse(msg, nfq_recv_valid_cb, qh)) < 0)
		xlog("nl_msg_parse: %s", nl_geterror(ret));

	return NL_STOP;
}

static int
nfq_overrun_cb(struct nl_msg *msg, void *arg)
{
	struct nfq_handler *qh = arg;

	pthread_mutex_lock(&qh->stats.nfqs_lock);
	qh->stats.nfqs_overrun++;
	pthread_mutex_unlock(&qh->stats.nfqs_lock);

	return NL_SKIP;
}

static int
nfq_err_cb(struct sockaddr_nl *sa, struct nlmsgerr *e, void *arg)
{
	struct nfq_handler *qh = arg;

	pthread_mutex_lock(&qh->stats.nfqs_lock);
	qh->stats.nfqs_err++;
	pthread_mutex_unlock(&qh->stats.nfqs_lock);

	return NL_SKIP;
}

static void
nfq_handler_stats_init(struct nfq_handler_stats *stats)
{
	pthread_mutex_init(&stats->nfqs_lock, NULL);
}

static int
nfq_handler_init(struct nfq_handler *qh)
{
	struct nl_cb *cb;
	int ret, opt, port;

	if ((qh->nfqh = nl_socket_alloc()) == NULL) {
		xlog("nl_handle_alloc: failed");
		return -1;
	}

	nl_socket_disable_seq_check(qh->nfqh);
	nl_socket_modify_cb(qh->nfqh, NL_CB_VALID, NL_CB_CUSTOM,
						nfq_event_cb, qh);
	nl_socket_modify_cb(qh->nfqh, NL_CB_OVERRUN, NL_CB_CUSTOM,
						nfq_overrun_cb, qh);

	if (verbose > 2) {
		port = nl_socket_get_local_port(qh->nfqh);
		xlog("q%d: using local port %d", qh->queueno, port);
	}

	/* setup error handler */
	cb = nl_socket_get_cb(qh->nfqh);
	nl_cb_err(cb, NL_CB_CUSTOM, nfq_err_cb, qh);

	if ((ret = nfnl_connect(qh->nfqh)) < 0)
		die("nfnl_connect: %s", nl_geterror(ret));

	/* set libnl internal receive buffer to the maximum in order to
	   keep working with defragment packets */
	ret = nl_socket_set_buffer_size(qh->nfqh, 64 * 1024, 64 * 1024);
	if (ret < 0) {
		xlog("nl_socket_set_buffer_size: %s", nl_geterror(ret));
		goto err;
	}

	/* disable ENOBUFS on socket */
	opt = 1;
	setsockopt(nl_socket_get_fd(qh->nfqh), SOL_NETLINK, NETLINK_NO_ENOBUFS,
			   &opt, sizeof(opt));

	nfnl_queue_pf_unbind(qh->nfqh, family);

	if ((ret = nfnl_queue_pf_bind(qh->nfqh, AF_INET)) < 0) {
		xlog("nfnl_queue_pf_bind: %s", nl_geterror(ret));
		return -1;
	}
	if ((ret = nfnl_queue_pf_bind(qh->nfqh, AF_INET6)) < 0) {
		xlog("nfnl_queue_pf_bind: %s", nl_geterror(ret));
		return -1;
	}

	if ((qh->nf_queue = nfnl_queue_alloc()) == NULL) {
		xlog("nfnl_queue_alloc: failed");
		return -1;
	}

	nfnl_queue_set_group(qh->nf_queue, qh->queueno);
	nfnl_queue_set_copy_mode(qh->nf_queue, NFNL_QUEUE_COPY_PACKET);
	nfnl_queue_set_copy_range(qh->nf_queue, 0xffff);
	if (g_queue_len > 0)
		nfnl_queue_set_maxlen(qh->nf_queue, qh->queue_len);

	if ((ret = nfnl_queue_create(qh->nfqh, qh->nf_queue)) < 0) {
		xlog("nfnl_queue_create: %s", nl_geterror(ret));
		return -1;
	}

	/* 1024 * 2048 = kernel_queue * avg(nfq_sg_size) */
	if (set_sockbuf_len(nl_socket_get_fd(qh->nfqh), 1024 * 2048) < 0)
		exit(1);

	list_add_tail(&qh->link, &nfq_handler_list);

	return 0;

err:
	nl_socket_free(qh->nfqh);
	return -1;
}

static struct nfq_handler *
nfq_handler_alloc(int queueno, int queue_len, int queue_batch)
{
	struct nfq_handler *qh;

	if ((qh = calloc(1, sizeof(struct nfq_handler))) == NULL) {
		xerr("out of memory");
		return NULL;
	}

	qh->queueno = queueno;
	qh->queue_len = queue_len;
	qh->queue_batch = queue_batch;
	qh->cpu = -1;
	nfq_handler_stats_init(&qh->stats);

	return qh;
}

void
nfq_handler_free(struct nfq_handler *nfqh)
{
	if (nfqh) {
		/* TODO free members */
		free(nfqh);
	}
}

static int
nfq_handler_main_loop(struct nfq_handler *qh)
{
	for (;;) {
		int nread;

		if ((nread = nl_recvmsgs_default(qh->nfqh)) < 0) {
			if (nread != ENOENT) {
				xerr("nl_recvmsgs: %s", nl_geterror(nread));
				return -1;
			}
		}
	}

	return 0;
}

static void
nfq_handler_dump_all(void)
{
	struct nfq_handler *qh;

	list_for_each_entry(qh, &nfq_handler_list, link) {
		struct nfq_handler_stats *stats = &qh->stats;

		pthread_mutex_lock(&stats->nfqs_lock);
		xlog("q[%d]: bytes=%llu pkts=%llu err=%llu ovrrun=%llu", qh->queueno,
			 stats->nfqs_bytes, stats->nfqs_pkts, stats->nfqs_err,
			 stats->nfqs_overrun);
		pthread_mutex_unlock(&stats->nfqs_lock);
	}
}

static int
single_queue(int queueno)
{
	struct nfq_handler *qh = nfq_handler_alloc(queueno, g_queue_len,
											   g_queue_batch);

	nfq_handler_init(qh);

	return nfq_handler_main_loop(qh);
}

static void *
nfq_worker_fn(void *arg)
{
	struct nfq_handler *qh = arg;

	if (nfq_handler_main_loop(qh) < 0)
		return NULL;

	return NULL;
}

static int
init_mt_queues(int firstq, int lastq)
{
	int queueno, cpu;

	xlog("initializing MT queues (%s affinity)",
		 affinity_first_cpu >= 0 ? "with" : "without");

	cpu = affinity_first_cpu;
	for (queueno = firstq; queueno <= lastq; queueno++, cpu++) {
		struct nfq_handler *qh = nfq_handler_alloc(queueno, g_queue_len,
												   g_queue_batch);
		int ret;

		if (!qh)
			die("out of memory");

		if (nfq_handler_init(qh) < 0)
			goto err;

		ret = pthread_create(&qh->thr.tid, NULL, nfq_worker_fn, qh);
		if (ret < 0)
			die("error creating thread: %s", strerror(ret));

		if (with_affinity()) {
			cpu_set_t cpuset;

			CPU_ZERO(&cpuset);
			CPU_SET(cpu, &cpuset);
			qh->cpu = cpu;
			ret = pthread_setaffinity_np(qh->thr.tid, sizeof(cpuset),
										 &cpuset);
			if (ret < 0) {
				xerr("pthread_setaffinity: %s", strerror(-ret));
				goto err;
			}
		}

		xlog("Initialized nfqueue %d (inet, inet6, len %d, batch %d)",
			 qh->queueno, qh->queue_len, qh->queue_batch);
	}

	return 0;

err:
	/* TODO cleanup */
	return -1;
}

static int
init_forked_queues(int firstq, int lastq)
{
	int queueno;

	xlog("initializing forked queues");

	for (queueno = firstq; queueno <= lastq; queueno++) {
		struct nfq_handler *qh = nfq_handler_alloc(queueno, g_queue_len,
												   g_queue_batch);
		pid_t child_pid;

		if (!qh)
			die("out of memory");

		if ((child_pid = fork()) == 0) { /* child */
			if (nfq_handler_init(qh) < 0)
				die("queue initialization failed");

			if (nfq_handler_main_loop(qh) < 0)
				return -1;
		}
	}

	return 0;
}

static void *
signal_fn(void *arg)
{
	sigset_t *ss = arg;

#if 0
	/* When using sigwait() the waited signals should be masked
	   in all threads, including the signal-handling thread. */
	if (pthread_sigmask(SIG_UNBLOCK, ss, NULL) < 0)
		die("error setting signal mask");
#endif

	for (;;) {
		int signo;

		if (sigwait(ss, &signo) < 0)
			xerr("sigwait: %m");
		switch (signo) {
		case SIGINT:
			/* TODO wait for threads to terminate */
			exit(0);

		case SIGTERM:
			/* TODO wait for threads to terminate */
			exit(0);

		case SIGUSR1:
			nfq_handler_dump_all();
			break;

		default:
			BUG();
		}
	}

	return NULL;
}

static void
signal_init(int poll_fd)
{
	pthread_t tid_sigs;
	sigset_t ss;
	int ret;

	sigemptyset(&ss);

    /* Specify the mask to be inherited by every other thread
	   by default.

	   Masking SIGFPE, SIGTRAP, SIGSEGV etc. is not necessary, as
	   they terminate the program anyway.

	   Specify only those signals which could race in their actions
	   with other stuff happening. */
#if 0
	sigaddset(&ss, SIGINT);
#endif
	sigaddset(&ss, SIGTERM);
	sigaddset(&ss, SIGALRM);
	sigaddset(&ss, SIGUSR1);
	if ((ret = pthread_sigmask(SIG_BLOCK, &ss, NULL)) != 0)
		die("error setting signal mask");

	if (pthread_create(&tid_sigs, NULL, signal_fn, &ss) < 0)
		die("error creating signal thread");
}

int
main(int argc, char *argv[])
{
	int poll_fd;

	if (check_opts(argc, argv) < 0)
		exit(1);

	if (geteuid() > 0)
		die("root required");

	if (do_debug == false) {
		if (daemon(0, 0) < 0)
			die("daemon failed: %m");
	}

	/* daemonized from here on */
	poll_fd = ev_init();
	signal_init(poll_fd);

	if (first_queueno == last_queueno)
		single_queue(first_queueno);
	else {
		if (is_forked)
			init_forked_queues(first_queueno, last_queueno);
		else
			init_mt_queues(first_queueno, last_queueno);
	}

	ev_dispatch();

	return 0;
}
