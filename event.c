/*
 * event.c
 *
 * Holger Eitzenberger, 2007.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <assert.h>
#include <sys/queue.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include "event.h"

struct ev_lh ev_list;
int epoll_fd;
struct epoll_event *events;


int
ev_init(void)
{
	epoll_fd = epoll_create(10);

	if ((events = calloc(10, sizeof(struct epoll_event))) == NULL)
		return -1;

	return epoll_fd;
}


struct ev *
ev_new(void)
{
	struct ev *ev;

	if ((ev = calloc(1, sizeof(struct ev))) == NULL)
		return NULL;

	return ev;
}


int
ev_add(struct ev *ev)
{
	struct epoll_event eev = {
		.data = { .ptr = ev, },
		.events = EPOLLIN,
	};

	if (!ev || ev->fd <= 0 || !ev->cb) {
		errno = EINVAL;

		return -1;
	}

	if ((epoll_ctl(epoll_fd, EPOLL_CTL_ADD, ev->fd, &eev)) < 0) {
		perror("epoll_ctl");
		return -1;
	}

	LIST_INSERT_HEAD(&ev_list, ev, link);

	return 0;
}


int
ev_dispatch(void)
{
	for (;;) {
		int i, nfds;

		nfds = epoll_wait(epoll_fd, events, 1, -1);

		for (i = 0; i < nfds; i++) {
			struct ev *ev = events[i].data.ptr;

			assert(ev->cb != NULL);

			ev->cb(ev, NULL);
		}
	}

	return 0;
}
