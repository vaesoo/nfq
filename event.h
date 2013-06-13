/*
 * event.h
 *
 * Holger Eitzenberger, 2007.
 */
#ifndef EVENT_H
#define EVENT_H

#include <sys/queue.h>


struct ev;

typedef int (* ev_callback_t)(struct ev *, void *);

struct ev {
	LIST_ENTRY(ev) link;
	int fd;
	ev_callback_t cb;
	void *arg;
};

LIST_HEAD(ev_lh, ev);


int ev_init(void);
struct ev *ev_new(void);
int ev_del(struct ev *);
int ev_add(struct ev *);
int ev_dispatch(void);

#endif /* EVENT_H */
