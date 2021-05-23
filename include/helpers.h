#ifndef HELPERS_H
#define HELPERS_H

typedef struct LinkedNode {
  void *data;
  struct LinkedNode *prev;
  struct LinkedNode *next;
} LinkedNode;

/*
 * Move element in item to the head of list mainlist.
 */
void list_move_head(LinkedNode **mainlist, LinkedNode *item);

/*
 * Create space for a new item and add it to the head of mainlist.
 *
 * Returns item or NULL if out of memory.
 */
LinkedNode *list_add(LinkedNode **mainlist);

/*
 * Delete item from list mainlist.
 */
void list_del(LinkedNode **mainlist, LinkedNode *item);

/*
 * Free any data in current item and then delete item. Optionally
 * update number of items in list if stored != NULL.
 */
void list_free(LinkedNode **list, int *stored, LinkedNode *item);

/*
 * Delete all items in list. Optionally update number of items in list
 * if stored != NULL.
 */
void list_del_all(LinkedNode **list, int *stored);

/*
 * Print all items in mainlist on stdout.
 */
void list_print(LinkedNode *mainlist);

#endif
