
#include <assert.h>
#include "list.h"


static void
__list_add(struct list_head *node, struct list_head *prev,
				 struct list_head *next)
{
	prev->next = node;
	node->prev = prev;
	next->prev = node;
	node->next = next;
}

void
list_add(struct list_head *node, struct list_head *head)
{
	__list_add(node, head, head->next);
}

void
list_add_tail(struct list_head *node, struct list_head *head)
{
	__list_add(node, head->prev, head);
}

void
list_del(struct list_head *node)
{
	node->next->prev = node->prev;
	node->prev->next = node->next;
}

/* move an entire list */
void
list_move_list(struct list_head *from, struct list_head *to)
{
	assert(list_empty(to));
	*to = *from;
	to->prev->next = to;
	to->next->prev = to;
	list_head_init(from);
}
