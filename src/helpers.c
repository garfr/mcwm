#include <stdlib.h>
#include <stdio.h>
#include "helpers.h"

#define TERMNRM "\x1B[0m"
#define TERMRED "\x1B[31m"
#define TERMBLU "\x1B[34m"

int verbose = 0;

/*
 * Move element in item to the head of list mainlist.
 */
void
list_move_head(LinkedNode **mainlist, LinkedNode *item) {
  if (NULL == item || NULL == mainlist || NULL == *mainlist) {
    return;
  }

  if (*mainlist == item) {
    /* item is NULL or we're already at head. Do nothing. */
    return;
  }

  /* Braid together the list where we are now. */
  if (NULL != item->prev) {
    item->prev->next = item->next;
  }

  if (NULL != item->next) {
    item->next->prev = item->prev;
  }

  /* Now we'at head, so no one before us. */
  item->prev = NULL;

  /* Old head is our next. */
  item->next = *mainlist;

  /* Old head needs to know about us. */
  item->next->prev = item;

  /* Remember the new head. */
  *mainlist = item;
}

/*
 * Create space for a new item and add it to the head of mainlist.
 *
 * Returns item or NULL if out of memory.
 */
LinkedNode *
list_add(LinkedNode **mainlist) {
  LinkedNode *item;

  if (NULL == (item = (LinkedNode *)malloc(sizeof(LinkedNode)))) {
    return NULL;
  }

  if (NULL == *mainlist) {
    /* First in the list. */

    item->prev = NULL;
    item->next = NULL;
  } else {
    /* Add to beginning of list. */

    item->next = *mainlist;
    item->next->prev = item;
    item->prev = NULL;
  }

  *mainlist = item;

  return item;
}

void
list_del(LinkedNode **mainlist, LinkedNode *item) {
  LinkedNode *ml = *mainlist;

  if (NULL == mainlist || NULL == *mainlist || NULL == item) {
    return;
  }

  if (item == *mainlist) {
    /* First entry was removed. Remember the next one instead. */
    *mainlist = ml->next;
  } else {
    item->prev->next = item->next;

    if (NULL != item->next) {
      /* This is not the last item in the list. */
      item->next->prev = item->prev;
    }
  }

  free(item);
}

void
list_free(LinkedNode **list, int *stored, LinkedNode *item) {
  if (NULL == list || NULL == *list || NULL == item) {
    return;
  }

  if (NULL != item->data) {
    free(item->data);
    item->data = NULL;
  }

  list_del(list, item);

  if (NULL != stored) {
    (*stored)--;
  }
}

/*
 * Delete all elements in list and free memory resources.
 */
void
list_del_all(LinkedNode **list, int *stored) {
  LinkedNode *item;
  LinkedNode *next;

  for (item = *list; item != NULL; item = next) {
    next = item->next;
    free(item->data);
    list_del(list, item);
  }

  if (NULL != stored) {
    (*stored) = 0;
  }
}

void
list_print(LinkedNode *mainlist) {
  LinkedNode *item;
  int i;

  for (item = mainlist, i = 1; item != NULL; item = item->next, i++) {
    printf("item #%d (stored at %p).\n", i, (void *)item);
  }
}

void
log_verbose(const char *fmt, ...) {
  if (verbose) {
    printf(TERMBLU "log: " TERMNRM);
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    printf("\n");
  }
}

void
log_err(const char *fmt, ...) {
  fprintf(stderr, TERMRED "error: " TERMNRM);
  va_list args;
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  fprintf(stderr, "\n");
}

void
log_err_final(const char *fmt, ...) {
  fprintf(stderr, TERMRED "error: " TERMNRM);
  va_list args;
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  fprintf(stderr, "\n");
  exit(EXIT_FAILURE);
}
