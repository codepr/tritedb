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

#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "trie.h"
#include "util.h"


/* Auxiliary comparison function, uses on list searches, this one compare the
 * char field stored in each struct trie_node structure contained in each node of the
 * list.
 */
static int with_char(void *arg1, void *arg2) {

    struct trie_node *tn1 = ((struct list_node *) arg1)->data;
    struct trie_node *tn2 = ((struct list_node *) arg2)->data;

    if (tn1->chr == tn2->chr)
        return 0;

    return -1;
}

// Check for children in a struct trie_node, if a node has no children is considered
// free
static bool trie_is_free_node(const struct trie_node *node) {
    return node->children->len == 0 ? true : false;
}


static struct trie_node *trie_node_find(const struct trie_node *node, const char *prefix) {

    struct trie_node *retnode = (struct trie_node *) node;

    // Move to the end of the prefix first
    for (; *prefix; prefix++) {

        // O(n), the best we can have
        struct list_node *child = linear_search(retnode->children, *prefix);

        // No key with the full prefix in the trie
        if (!child)
            return NULL;

        retnode = child->data;
    }

    return retnode;
}


static int trie_node_count(const struct trie_node *node) {

    /* Count only if it is a leaf with a value */
    if (trie_is_free_node(node) && node->ndata && node->ndata->data)
        return 1;

    int count = 0;

    /* Recurse through all the children */
    for (struct list_node *cur = node->children->head; cur; cur = cur->next)
        count += trie_node_count(cur->data);

    /* Add count if the node has a value */
    if (node->ndata)
        count++;

    return count;
}


// Returns new trie node (initialized to NULL)
struct trie_node *trie_create_node(char c) {

    struct trie_node *new_node = tmalloc(sizeof(*new_node));

    if (new_node) {

        new_node->chr = c;
        new_node->ndata = NULL;
        new_node->children = list_create(NULL);
    }

    return new_node;
}

// Returns new Trie, with a NULL root and 0 size
Trie *trie_create(void) {
    Trie *trie = tmalloc(sizeof(*trie));
    trie->root = trie_create_node(' ');
    trie->size = 0;
    return trie;
}


size_t trie_size(const Trie *trie) {
    return trie->size;
}

/* If not present, inserts key into trie, if the key is prefix of trie node,
   just marks leaf node by assigning the new data pointer. Returns a pointer
   to the new inserted data.

   Being a Trie, it should guarantees O(m) performance for insertion on the
   worst case, where `m` is the length of the key. */
static struct node_data *trie_node_insert(struct trie_node *root,
        const char *key, const void *data, size_t *size) {

    struct trie_node *cursor = root;
    struct trie_node *cur_node = NULL;
    struct list_node *tmp = NULL;

    // Iterate through the key char by char
    for (; *key; key++) {

        /* We can use a linear search as on a linked list O(n) is the best find
         * algorithm we can use, as binary search would have the same if not
         * worse performance by not having direct access to node like in an
         * array.
         *
         * Anyway we expect to have an average O(n/2) cause at every insertion
         * the list is sorted so we expect to find our char in the middle on
         * average.
         *
         * As a future improvement it's advisable to substitute list with a
         * B-tree or RBTree to improve searching complexity to O(logn) at best,
         * avg and worst while maintaining O(n) space complexity, but it really
         * depends also on the size of the alphabet.
         */
        tmp = linear_search(cursor->children, *key);

        // No match, we add a new node and sort the list with the new added link
        if (!tmp) {
            cur_node = trie_create_node(*key);
            cursor->children = list_push(cursor->children, cur_node);
            cursor->children->head = merge_sort_tnode(cursor->children->head);
        } else {
            // Match found, no need to sort the list, the child already exists
            cur_node = tmp->data;
        }
        cursor = cur_node;
    }

    /* Clear out if already taken (e.g. we are in a leaf node), rc = 0 to not
     * change the trie size, otherwise 1 means that we added a new node,
     * effectively changing the size
     */
    if (cursor->ndata) {
        tfree(cursor->ndata->data);
    } else {
        (*size)++;
        cursor->ndata = tmalloc(sizeof(struct node_data));
    }

    // mark last node as leaf
    cursor->ndata->data = tstrdup(data);
    cursor->ndata->ttl = -NOTTL;
    cursor->ndata->ctime = cursor->ndata->latime = time(NULL);

    return cursor->ndata;
}

/* Private function, iterate recursively through the trie structure starting
   from a given node, deleting the target value */
static bool trie_node_recursive_delete(struct trie_node *node,
        const char *key, size_t *size, bool *found) {

    if (!node)
        return false;

    // Base case
    if (*key == '\0') {

        if (node->ndata) {

            // Update found flag
            *found = true;

            // Free resources, covering the case of a sub-prefix
            if (node->ndata->data) {
                tfree(node->ndata->data);
                node->ndata->data = NULL;
            }
            tfree(node->ndata);
            node->ndata = NULL;
            if (*size > 0)
                (*size)--;

            // If empty, node to be deleted
            return trie_is_free_node(node);
        }

    } else {

        // O(n), the best we can have
        struct list_node *cur = linear_search(node->children, *key);

        if (!cur)
            return false;

        struct trie_node *child = cur->data;

        if (trie_node_recursive_delete(child, key + 1, size, found)) {

            // Messy solution, requiring probably avoidable allocations
            struct trie_node t = {*key, NULL, NULL};
            struct list_node tmp = {&t, NULL};
            list_remove(node->children, &tmp, with_char);

            // last node marked, delete it
            trie_node_free(child, size);

            // recursively climb up, and delete eligible nodes
            return (!node->ndata && trie_is_free_node(node));
        }
    }

    return false;
}

/* Returns true if key is present in trie, else false. Also for lookup the
   big-O runtime is guaranteed O(m) with `m` as length of the key. */
static bool trie_node_search(const struct trie_node *root,
        const char *key, void **ret) {

    // Walk the trie till the end of the key
    struct trie_node *cursor = trie_node_find(root, key);

    *ret = (cursor && cursor->ndata) ? cursor->ndata : NULL;

    // Return false if no complete key found, true otherwise
    return !*ret ? false : true;
}

/* Insert a new key-value pair in the Trie structure, returning a pointer to
   the new inserted data in order to simplify some operations as the addition
   of expiring keys with a set TTL. */
struct node_data *trie_insert(Trie *trie, const char *key, const void *data) {

    assert(trie && key);

    return trie_node_insert(trie->root, key, data, &trie->size);
}


bool trie_delete(Trie *trie, const char *key) {

    assert(trie && key);

    bool found = false;

    if (strlen(key) > 0)
        trie_node_recursive_delete(trie->root, key, &(trie->size), &found);

    return found;
}


bool trie_find(const Trie *trie, const char *key, void **ret) {
    assert(trie && key);
    return trie_node_search(trie->root, key, ret);
}

/* Remove and delete all keys matching a given prefix in the trie
   e.g. hello*
   - hello
   hellot
   helloworld
   hello
   */
void trie_prefix_delete(Trie *trie, const char *prefix) {

    assert(trie && prefix);

    // Walk the trie till the end of the key
    struct trie_node *cursor = trie_node_find(trie->root, prefix);

    // No complete key found
    if (!cursor)
        return;

    // Simply remove the key if it has no children, no need to clear the list
    if (cursor->children->len == 0) {
        trie_delete(trie, prefix);
        return;
    }

    // Clear out all possible sub-paths
    for (struct list_node *cur = cursor->children->head; cur; cur = cur->next) {
        trie_node_free(cur->data, &(trie->size));
        cur->data = NULL;
    }

    // Set the current node (the one storing the last character of the prefix)
    // as a leaf and delete the prefix key as well
    trie_delete(trie, prefix);

    list_clear(cursor->children, 1);
}


int trie_prefix_count(const Trie *trie, const char *prefix) {

    assert(trie && prefix);

    int count = 0;

    // Walk the trie till the end of the key
    struct trie_node *node = trie_node_find(trie->root, prefix);

    // No complete key found
    if (!node)
        return count;

    // Check all possible sub-paths and add to count where there is a leaf */
    count += trie_node_count(node);

    return count;
}

/* Auxiliary function to modify trie values only if they're effectively
 * integers, by adding a quantity or subtracting it */
static void trie_node_integer_mod(struct trie_node *node, int value, bool inc) {

    if (trie_is_free_node(node) && !node->ndata)
        return;

    if (node->ndata && is_integer(node->ndata->data)) {
        int n = parse_int(node->ndata->data);
        n = inc == true ? n + 1 : n - 1;
        // Check for realloc if the new value is "larger" then previous
        char tmp[12];  // max size in bytes
        sprintf(tmp, "%d", n);  // XXX Unsafe
        size_t len = strlen(tmp);
        node->ndata->data = trealloc(node->ndata->data, len + 1);
        strncpy(node->ndata->data, tmp, len + 1);
        node->ndata->latime = time(NULL);
    }

    for (struct list_node *cur = node->children->head; cur; cur = cur->next)
        trie_node_integer_mod(((struct trie_node *) cur->data), value, inc);
}

// Add 1 to all integer values matching a given prefix
void trie_prefix_inc(Trie *trie, const char *prefix) {

    assert(trie && prefix);

    // Walk the trie till the end of the key
    struct trie_node *node = trie_node_find(trie->root, prefix);

    // No complete key found
    if (!node)
        return;

    // Check all possible sub-paths and add to count where there is a leaf
    trie_node_integer_mod(node, 1, true);
}

// Subtract 1 to all integer values matching a given prefix
void trie_prefix_dec(Trie *trie, const char *prefix) {

    assert(trie && prefix);

    // Walk the trie till the end of the key
    struct trie_node *node = trie_node_find(trie->root, prefix);

    // No complete key found
    if (!node)
        return;

    // Check all possible sub-paths and add to count where there is a leaf
    trie_node_integer_mod(node, 1, false);
}


static void trie_node_prefix_set(struct trie_node *node,
        const void *val, int16_t ttl) {

    if (!node)
        return;

    for (struct list_node *cur = node->children->head; cur; cur = cur->next)
        trie_node_prefix_set(cur->data, val, ttl);

    // mark last node as leaf
    if (node->ndata && node->ndata->data) {
        tfree(node->ndata->data);
        node->ndata->data = tstrdup(val);
        node->ndata->ttl = ttl;
        node->ndata->latime = time(NULL);
    }
}


void trie_prefix_set(Trie *trie,
        const char *prefix, const void *val, int16_t ttl) {

    assert(trie && prefix);

    // Walk the trie till the end of the key
    struct trie_node *node = trie_node_find(trie->root, prefix);

    // No complete key found
    if (!node)
        return;

    // Check all possible sub-paths and add to count where there is a leaf
    trie_node_prefix_set(node, val, ttl);
}


static void trie_node_prefix_ttl(struct trie_node *node, int16_t ttl) {

    if (!node)
        return;

    for (struct list_node *cur = node->children->head; cur; cur = cur->next)
        trie_node_prefix_ttl(cur->data, ttl);

    // mark last node as leaf
    if (node->ndata && node->ndata->data) {
        node->ndata->ttl = ttl;
        node->ndata->latime = time(NULL);
    }
}


void trie_prefix_ttl(Trie *trie, const char *prefix, int16_t ttl) {

    assert(trie && prefix);

    // Walk the trie till the end of the key
    struct trie_node *node = trie_node_find(trie->root, prefix);

    // No complete key found
    if (!node)
        return;

    // Check all possible sub-paths and add to count where there is a leaf
    trie_node_prefix_ttl(node, ttl);
}


static void trie_node_prefix_find(const struct trie_node *node,
        char str[], int level, List *keys) {

    // If node is leaf node, it indicates end of string, so a null charcter is
    // added and string is added to the keys list
    if (node && node->ndata && node->ndata->data) {
        str[level] = '\0';
        list_push_back(keys, tstrdup(str));
    }

    for (struct list_node *cur = node->children->head; cur; cur = cur->next) {
        // if NON NULL child is found add parent key to str and call the
        // function recursively for child node, caring for the size of the
        // current string, if exceed bounds, double the size of the string
        // host
        if (level == malloc_size(str))
            str = trealloc(str, level * 2);
        str[level] = ((struct trie_node *) cur->data)->chr;
        trie_node_prefix_find(cur->data, str, level + 1, keys);
    }
}


List *trie_prefix_find(const Trie *trie, const char *prefix) {

    assert(trie && prefix);

    // Walk the trie till the end of the key
    struct trie_node *node = trie_node_find(trie->root, prefix);

    // No complete key found
    if (!node)
        return NULL;

    List *keys = list_create(NULL);

    // Check all possible sub-paths and add the resulting key to the result
    char *str = tmalloc(32);
    size_t plen = strlen(prefix);
    strcpy(str, prefix);

    // Recursive function call
    trie_node_prefix_find(node, str, plen, keys);

    tfree(str);

    return keys;
}

/* Iterate through children of each node starting from a given node, applying
   a defined function which take a struct trie_node as argument */
static void trie_prefix_map_func(struct trie_node *node, void (*mapfunc)(struct trie_node *)) {

    if (trie_is_free_node(node)) {
        mapfunc(node);
        return;
    }

    for (struct list_node *child = node->children->head; child; child = child->next)
        trie_prefix_map_func(child->data, mapfunc);

    mapfunc(node);

}

/* Apply a function to every key below a given prefix, if prefix is null the
   function will be applied to all the trie */
void trie_prefix_map(Trie *trie,
        const char *prefix, void (*mapfunc)(struct trie_node *)) {

    assert(trie);

    if (!prefix) {
        trie_prefix_map_func(trie->root, mapfunc);
    } else {

        // Walk the trie till the end of the key
        struct trie_node *node = trie_node_find(trie->root, prefix);

        // No complete key found
        if (!node)
            return;

        // Check all possible sub-paths and add to count where there is a leaf
        trie_prefix_map_func(node, mapfunc);
    }
}

/* Release memory of a node while updating size of the trie */
void trie_node_free(struct trie_node *node, size_t *size) {

    // Base case
    if (!node)
        return;

    // Recursive call to all children of the node
    if (node->children) {
        for (struct list_node *cur = node->children->head; cur; cur = cur->next)
            trie_node_free(cur->data, size);
        list_release(node->children, 0);
        node->children = NULL;
    }

    // Release memory on data stored on the node
    if (node->ndata && node->ndata->data) {
        tfree(node->ndata->data);
        tfree(node->ndata);
        if (*size > 0)
            (*size)--;
    } else if (node->ndata) {
        tfree(node->ndata);
        if (*size > 0)
            (*size)--;
    }

    // Release the node itself
    tfree(node);
}


void trie_release(Trie *trie) {

    if (!trie)
        return;

    trie_node_free(trie->root, &(trie->size));

    tfree(trie);
}
