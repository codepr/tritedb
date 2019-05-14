/* BSD 2-Clause License
 *
 * Copyright (c) 2018, Andrea Giacomo Baldan
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdlib.h>
#include "list.h"
#include "util.h"
#include "server.h"


static struct list_node *list_node_remove(struct list_node *,
                                          struct list_node *,
                                          compare_func, int *);

/*
 * Create a list, initializing all fields
 */
List *list_new(int (*destructor)(struct list_node *)) {

    List *l = tmalloc(sizeof(List));

    if (!l) {
        perror("malloc(3) failed");
        exit(EXIT_FAILURE);
    }

    // set default values to the List structure fields
    l->head = l->tail = NULL;
    l->len = 0L;
    // TODO if NULL set default destructor
    l->destructor = destructor;

    return l;
}


/*
 * Destroy a list, releasing all allocated memory
 */
void list_destroy(List *l, int deep) {

    if (!l) return;

    struct list_node *h = l->head;
    struct list_node *tmp;

    // free all nodes
    while (l->len--) {

        tmp = h->next;

        if (l->destructor)
            l->destructor(h);
        else {
            if (h) {
                if (h->data && deep == 1)
                    tfree(h->data);
                tfree(h);
            }
        }

        h = tmp;
    }

    // free List structure pointer
    tfree(l);
}


unsigned long list_size(const List *list) {
    return list->len;
}

/*
 * Destroy a list, releasing all allocated memory but the list itself
 */
void list_clear(List *l, int deep) {

    if (!l || !l->head) return;

    struct list_node *h = l->head;
    struct list_node *tmp;

    // free all nodes
    while (l->len--) {

        tmp = h->next;

        if (h) {
            if (h->data && deep == 1) tfree(h->data);
            tfree(h);
        }

        h = tmp;
    }

    l->head = l->tail = NULL;
    l->len = 0L;
}

/*
 * Attach a node to the head of a new list
 */
List *list_attach(List *l, struct list_node *head, unsigned long len) {
    // set default values to the List structure fields
    l->head = head;
    l->len = len;
    return l;
}

/*
 * Insert value at the front of the list
 * Complexity: O(1)
 */
List *list_push(List *l, void *val) {

    struct list_node *new_node = tmalloc(sizeof(struct list_node));

    if (!new_node) {
        perror("malloc(3) failed");
        exit(EXIT_FAILURE);
    }

    new_node->data = val;

    if (l->len == 0) {
        l->head = l->tail = new_node;
        new_node->next = NULL;
    } else {
        new_node->next = l->head;
        l->head = new_node;
    }

    l->len++;

    return l;
}


/*
 * Insert value at the back of the list
 * Complexity: O(1)
 */
List *list_push_back(List *l, void *val) {

    struct list_node *new_node = tmalloc(sizeof(struct list_node));

    if (!new_node) {
        perror("malloc(3) failed");
        exit(EXIT_FAILURE);
    }

    new_node->data = val;
    new_node->next = NULL;

    if (l->len == 0) {
        l->head = l->tail = new_node;
    } else {
        l->tail->next = new_node;
        l->tail = new_node;
    }

    l->len++;

    return l;
}


void list_remove(List *l, struct list_node *node, compare_func cmp) {

    if (!l || !node)
        return;

    int counter = 0;

    l->head = list_node_remove(l->head, node, cmp, &counter);

    l->len -= counter;

}


static struct list_node *list_node_remove(struct list_node *head,
                                          struct list_node *node,
                                          compare_func cmp, int *counter) {

    if (!head)
        return NULL;

    if (cmp(head, node) == 0) {

        struct list_node *tmp_next = head->next;
        tfree(head);
        head = NULL;

        // Update remove counter
        (*counter)++;

        return tmp_next;
    }

    head->next = list_node_remove(head->next, node, cmp, counter);

    return head;
}


static struct list_node *list_remove_single_node(struct list_node *head,
                                                 void *data,
                                                 struct list_node **ret,
                                                 compare_func cmp) {

    if (!head)
        return NULL;

    // We want the first match
    if (cmp(head, data) == 0 && !*ret) {

        struct list_node *tmp_next = head->next;

        *ret = head;

        return tmp_next;

    }

    head->next = list_remove_single_node(head->next, data, ret, cmp);

    return head;

}


struct list_node *list_remove_node(List *list, void *data, compare_func cmp) {

    if (list->len == 0 || !list)
        return NULL;

    struct list_node *node = NULL;

    list_remove_single_node(list->head, data, &node, cmp);

    if (node) {
        list->len--;
        node->next = NULL;
    }

    return node;
}

/*
 * Returns a pointer to a node near the middle of the list,
 * after having truncated the original list before that point.
 */
static struct list_node *bisect_list(struct list_node *head) {
    /* The fast pointer moves twice as fast as the slow pointer. */
    /* The prev pointer points to the node preceding the slow pointer. */
    struct list_node *fast = head, *slow = head, *prev = NULL;

    while (fast != NULL && fast->next != NULL) {
        fast = fast->next->next;
        prev = slow;
        slow = slow->next;
    }

    if (prev != NULL)
        prev->next = NULL;

    return slow;
}

/*
 * Merges two list by using a comparison function, sorting them according to
 * the outcome of the given comparison func.
 */
static struct list_node *merge_list(struct list_node *list1,
                                    struct list_node *list2, cmp cmp_func) {

    struct list_node dummy_head = { NULL, NULL }, *tail = &dummy_head;

    while (list1 && list2) {

        int outcome = cmp_func(list1->data, list2->data);

        struct list_node **min = outcome <= 0 ? &list1 : &list2;
        struct list_node *next = (*min)->next;
        tail = tail->next = *min;
        *min = next;
    }

    tail->next = list1 ? list1 : list2;
    return dummy_head.next;
}

/*
 * Merge sort for nodes list, apply a given comparison function which return
 * -1 0 or 1 based on the outcome of the comparison.
 */
struct list_node *list_merge_sort(struct list_node *head, cmp cmp_func) {

    struct list_node *list1 = head;

    if (!list1 || !list1->next)
        return list1;

    /* find the middle */
    struct list_node *list2 = bisect_list(list1);

    return merge_list(list_merge_sort(list1, cmp_func),
                      list_merge_sort(list2, cmp_func), cmp_func);
}

/* Search for a given node based on a comparison of char stored in structure
 * and a value, O(n) at worst
 */
struct list_node *linear_search(const List *list, int value) {

    if (!list || list->len == 0)
        return NULL;

    for (struct list_node *cur = list->head; cur != NULL; cur = cur->next) {
        if (((struct trie_node *) cur->data)->chr == value)
            return cur;
        else if (((struct trie_node *) cur->data)->chr > value)
            break;
    }

    return NULL;
}


static struct list_node *merge_tnode_list(struct list_node *list1,
                                          struct list_node *list2) {

    struct list_node dummy_head = { NULL, NULL }, *tail = &dummy_head;

    while (list1 && list2) {

        /* cast to cluster_node */
        char chr1 = ((struct trie_node *) list1->data)->chr;
        char chr2 = ((struct trie_node *) list2->data)->chr;

        struct list_node **min = chr1 <= chr2 ? &list1 : &list2;
        struct list_node *next = (*min)->next;
        tail = tail->next = *min;
        *min = next;
    }

    tail->next = list1 ? list1 : list2;
    return dummy_head.next;
}


struct list_node *merge_sort_tnode(struct list_node *head) {

    struct list_node *list1 = head;

    if (!list1 || !list1->next)
        return list1;

    /* find the middle */
    struct list_node *list2 = bisect_list(list1);

    return merge_tnode_list(merge_sort_tnode(list1), merge_sort_tnode(list2));
}
