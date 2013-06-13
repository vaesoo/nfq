#ifndef LIST_H
#define LIST_H

#include <stdbool.h>

struct list_head {
	struct list_head *next;
	struct list_head *prev;
};

/* FIXME there seems to be a clash with the <sys/queue.h> header
  when being named just 'LIST_HEAD'. */
#define MY_LIST_HEAD(name)	struct list_head (name) = { &(name), &(name) };

static inline void list_head_init(struct list_head *list)
{
	list->prev = list->next = list;
}

static inline bool list_empty(const struct list_head *head)
{
	return head->next == head;
}

static inline struct list_head *
list_next(struct list_head *node)
{
	return node->next;
}

#define container_of(ptr, type, member)	({ \
	(type *)((char *)(ptr) - ((size_t)&((type *)0)->member)); })

#define list_entry(ptr, type, member) \
	container_of(ptr, type, member)

#define list_for_each_entry(i, head, memb)								\
	for (i = list_entry((head)->next, typeof(*i), memb); &i->memb != head; \
		 i = list_entry(i->memb.next, typeof(*i), memb))

#define list_for_each_entry_reverse(i, head, memb)						\
	for (i = list_entry((head)->prev, typeof(*i), memb); &i->memb != head; \
		 i = list_entry(i->memb.prev, typeof(*i), memb))

#define list_for_each_entry_safe(i, n, head, memb)		 \
	for (i = list_entry((head)->next, typeof(*i), memb), \
			n = list_entry(i->memb.next, typeof(*i), memb); \
		&i->memb != head; \
		i = n, n = list_entry(n->memb.next, typeof(*i), memb))


void list_add(struct list_head *node, struct list_head *head);
void list_add_tail(struct list_head *node, struct list_head *head);
void list_del(struct list_head *node);
void list_move_list(struct list_head *, struct list_head *);

#endif /* LIST_H */
