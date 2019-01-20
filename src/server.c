/* BSD 2-Clause License
 *
 * Copyright (c) 2018, Andrea Giacomo Baldan All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <time.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <uuid/uuid.h>
#include "list.h"
#include "util.h"
#include "server.h"
#include "config.h"
#include "network.h"
#include "protocol.h"


/*
 * Helper macro to create a join request to a defined seed node, communicating
 * address and port
 */
#define make_join_request(addr, port, flags)                    \
    make_keyval_request((const uint8_t *) addr,                 \
            (const uint8_t *) port, CLUSTER_JOIN, 0x00, flags); \

/*
 * Helper macro to set an ACK reply, just a normal reply with only a return
 * code for responding to commands like PUT or DEL, stating the result of the
 * operation
 */
#define set_ack_reply(c, o, t, f) do {                                      \
    struct response *response = make_ack_response((o), (t), (f));           \
    struct buffer *buffer = buffer_init(response->ncontent->header->size);  \
    pack_response(buffer, response);                                        \
    set_reply((c), buffer, -1);                                             \
    free_response(response);                                                \
} while (0)

/* Global information structure */
static struct informations info;

/* The main TriteDB instance */
static struct tritedb tritedb;

/*
 * Reply structure, contains the file descriptor of a connected client and a
 * pointer to a buffer structure which contains the payload to be sent through
 * the socket and his length
 */
struct reply {
    int fd;
    struct buffer *payload;
};

/*
 * Command handler helper structure, just for mapping
 * command type -> command handler functions easily
 */
struct command_handler {
    int ctype;
    int (*handler)(struct client *);
};

/*
 * Connection structure for private use of the module, mainly for accepting
 * new connections
 */
struct connection {
    char ip[INET_ADDRSTRLEN + 1];
    int fd;
};

/*
 * General context handler functions, with the exception of free_reply which
 * is just an helper function to deallocate a reply structure, all of them
 * should be associated to a requesting client on each different step of
 * execution, being them ACCEPT -> REQUST -> REPLY
 */
static void free_reply(struct reply *);
static int reply_handler(struct client *);
static int accept_handler(struct client *);
static int accept_node_handler(struct client *);
static int request_handler(struct client *);
static int route_command(struct request *, struct buffer *, struct client *);

/* Specific handlers for commands that every client can request */
static int ack_handler(struct client *);
static int put_handler(struct client *);
static int get_handler(struct client *);
static int del_handler(struct client *);
static int ttl_handler(struct client *);
static int inc_handler(struct client *);
static int dec_handler(struct client *);
static int use_handler(struct client *);
static int cluster_join_handler(struct client *);
static int cluster_members_handler(struct client *);
static int ping_handler(struct client *);
static int db_handler(struct client *);
static int count_handler(struct client *);
static int keys_handler(struct client *);
static int info_handler(struct client *);
static int quit_handler(struct client *);

/*
 * Fixed size of the header of each packet, consists of essentially the first
 * 5 bytes containing respectively the type of packet (PUT, GET, DEL etc ...)
 * the total length in bytes of the packet and the is_bulk flag which tell if
 * the packet contains a stream of commands or a single one
 */
static const int HEADLEN = (2 * sizeof(uint8_t)) + sizeof(uint32_t);

/* Static command map, simple as it seems: OPCODE -> handler func */
static struct command_handler commands_map[COMMAND_COUNT] = {
    {ACK, ack_handler},
    {PUT, put_handler},
    {GET, get_handler},
    {DEL, del_handler},
    {TTL, ttl_handler},
    {INC, inc_handler},
    {DEC, dec_handler},
    {COUNT, count_handler},
    {KEYS, keys_handler},
    {USE, use_handler},
    {CLUSTER_JOIN, cluster_join_handler},
    {CLUSTER_MEMBERS, cluster_members_handler},
    {PING, ping_handler},
    {DB, db_handler},
    {INFO, info_handler},
    {QUIT, quit_handler}
};

/*
 * Parse header, require at least the first 5 bytes in order to read packet
 * type and total length that we need to recv to complete the packet
 */
struct buffer *recv_packet(int clientfd,
        Ringbuffer *rbuf, uint8_t *opcode, int *rc) {

    size_t n = 0;
    uint8_t read_all = 0;

    while (n < HEADLEN) {
        /* Read first 5 bytes to get the total len of the packet */
        n += recvbytes(clientfd, rbuf, read_all, HEADLEN);
        if (n <= 0) {
            *rc = -ERRCLIENTDC;
            return NULL;
        }
    }

    uint8_t *tmp = tmalloc(ringbuf_size(rbuf));
    uint8_t *bytearray = tmp;

    /* Try to read at least length of the packet */
    for (uint8_t i = 0; i < HEADLEN; i++)
        ringbuf_pop(rbuf, bytearray++);

    /* Read opcode, the first byte of every packet */
    uint8_t *opc = (uint8_t *) tmp;

    /* If opcode is not known close the connection */
    bool ok = false;
    for (int i = 0; i < COMMAND_COUNT && !ok; i++)
        if (*opc == commands_map[i].ctype || *opc == ACK)
            ok = true;

    if (!ok)
        goto errrecv;

    /* Read the total length of the packet */
    uint32_t tlen = ntohl(*((uint32_t *) (tmp + sizeof(uint8_t))));

    /*
     * Set return code to -ERRMAXREQSIZE in case of total packet len exceed
     * the configuration limit `max_request_size`
     */
    if (tlen > conf->max_request_size) {
        *rc = -ERRMAXREQSIZE;
        goto err;
    }

    /* Read flags */
    uint8_t flags = *((uint8_t *) (tmp + sizeof(uint8_t) + sizeof(uint32_t)));

    *rc = flags & F_FROMNODERESPONSE ? 1 : 0;

    /* Read remaining bytes to complete the packet */
    while (ringbuf_size(rbuf) < tlen - HEADLEN)
        if ((n = recvbytes(clientfd, rbuf, read_all, tlen - HEADLEN)) < 0)
            goto errrecv;

    /* Allocate a buffer to fit the entire packet */
    struct buffer *b = buffer_init(tlen);

    /* Copy previous read part of the header (first 6 bytes) */
    memcpy(b->data, tmp, HEADLEN);

    /* Move forward pointer after HEADLEN bytes */
    bytearray = b->data + HEADLEN;

    /* Empty the rest of the ring buffer */
    while ((tlen - HEADLEN) > 0) {
        ringbuf_pop(rbuf, bytearray++);
        --tlen;
    }

    *opcode = *opc;

    tfree(tmp);

    return b;

errrecv:

    shutdown(clientfd, 0);
    close(clientfd);

err:

    tfree(tmp);

    return NULL;
}

/*
 * Build a reply object and link it to the struct client pointer. Even tho it
 * removes the const qualifier from the struct buffer pointed by the ptr as a
 * matter of fact it doesn't touch it, so it is semantically correct to mark
 * it as const in the declaration.
 *
 * As last parameter it accepts a file descriptor which can be optionally set
 * to a negative value (-1 std) to fallback to the client fd; this to handle
 * communication from connected clients or from other tritedb nodes in a
 * cluster.
 */
static inline void set_reply(struct client *client,
        const struct buffer *payload, int fd) {

    struct reply *rep = tmalloc(sizeof(*rep));

    if (!rep)
        oom("setting reply");

    rep->fd = fd > 0 ? fd : client->fd;
    rep->payload = (struct buffer *) payload;

    client->reply = rep;
}


static inline void free_reply(struct reply *r) {

    if (!r)
        return;

    if (r->payload)
        buffer_destroy(r->payload);

    tfree(r);
}

/* List destructor function for struct keyval objects */
static inline int keyval_list_free(struct list_node *ln) {

    if (!ln)
        return -1;

    if (ln->data) {
        struct keyval *kv = ln->data;
        tfree(kv->key);
        tfree(kv->val);
    }

    tfree(ln->data);
    tfree(ln);

    return 1;
}

/* Queue destructor function for struct keyval objects. */
static inline int keyval_queue_free(struct queue_item *qitem) {

    if (!qitem)
        return -1;

    keyval_queue_free(qitem->next);

    if (qitem->data) {
        struct keyval *kv = qitem->data;
        tfree(kv->key);
        tfree(kv->val);
    }

    tfree(qitem->data);
    tfree(qitem);

    return 1;
}


static inline void client_free(struct client *client) {

    if (client->addr)
        tfree((char *) client->addr);

    if (client->reply)
        free_reply(client->reply);

    tfree(client);
}

/* Hashtable destructor function for struct client objects. */
static inline int hashtable_client_free(struct hashtable_entry *entry) {

    if (!entry || !entry->val)
        return -HASHTABLE_ERR;

    struct client *c = entry->val;

    client_free(c);

    return HASHTABLE_OK;
}

/*
 * Hashtable destructor function for struct database objects. It's the
 * function that will be called on hashtable_del call as well as
 * hashtable_release too.
 */
static inline int database_free(struct hashtable_entry *entry) {

    if (!entry)
        return -HASHTABLE_ERR;

    tfree((char *) ((struct database *) entry->val)->name);

    trie_release(((struct database *) entry->val)->data);

    tfree(entry->val);
    tfree((char *) entry->key);

    return HASHTABLE_OK;
}


/********************************/
/*      COMMAND HANDLERS        */
/********************************/


static int ack_handler(struct client *c) {

    int ret = 5;

    if (!(c->response->ncontent->header->flags & F_FROMNODERESPONSE))
        goto exit;

    /*
     * Check if there are pending members in queue for hash ring adding and in
     * case add a connection request to the next one and add it to the ring
     */
    if (!queue_empty(tritedb.pending_members)) {

        struct keyval *pair = queue_get(tritedb.pending_members);

        // Connect to the first node target
        int port = atoi((const char *) pair->val) + 10000;

        int fd = open_connection((const char *) pair->key, port);

        // Create a new client for it
        if (set_nonblocking(fd) < 0)
            perror("set_nonblocking: ");

        if (set_tcp_nodelay(fd) < 0)
            perror("set_tcp_nodelay: ");

        struct client *new_node = tmalloc(sizeof(*new_node));
        new_node->ctype = NODE;
        new_node->addr = tstrdup((const char *) pair->key);
        new_node->fd = fd;
        new_node->last_action_time = time(NULL);
        new_node->ctx_handler = reply_handler;
        new_node->reply = NULL;
        new_node->request = NULL;
        new_node->response = NULL;
        new_node->db = hashtable_get(tritedb.dbs, "db0");

        generate_uuid((char *) new_node->uuid);

        if (add_epoll(tritedb.epollfd, fd, EPOLLOUT, new_node) < 0)
            perror("epoll_add: ");

        /*
         * Add new node to the hashring of this instance, key field of the
         * command structure should carry the host+port string joined together
         */
        cluster_add_new_node(tritedb.cluster, new_node,
                (const char *) pair->key, (const char *) pair->val, false);

        /* Track the new connected node */
        hashtable_put(tritedb.nodes, new_node->uuid, new_node);

        tdebug("[ACK] Added %s:%s", pair->key, pair->val);

        // Send CLUSTER_JOIN request
        // XXX Obnoxious
        struct request *request =
            make_join_request(conf->hostname, conf->port, F_FROMNODEREQUEST);

        struct buffer *buffer =
            buffer_init(request->command->kvcommand->header->size);

        pack_request(buffer, request, KEY_VAL_COMMAND);

        set_reply(new_node, buffer, -1);

        free_request(request);

        // Update informations
        info.nconnections++;
        info.nnodes++;

        // XXX check
        tfree(pair->key);
        tfree(pair->val);
        tfree(pair);

        ret = OK;
    }

exit:

    free_response(c->response);

    return ret;
}


static int quit_handler(struct client *c) {

    tdebug("Closing connection with %s", c->addr);
    del_epoll(tritedb.epollfd, c->fd);
    shutdown(c->fd, 0);
    close(c->fd);
    info.nclients--;

    free_request(c->request);

    // Remove client from the clients map
    hashtable_del(tritedb.clients, c->uuid);

    return -1;
}


static int ping_handler(struct client *c) {

    tdebug("PING from %s", c->addr);

    // TODO send out a PONG
    set_ack_reply(c, OK, NULL, F_NOFLAG);

    free_request(c->request);

    return OK;
}


static int info_handler(struct client *c) {

    info.uptime = time(NULL) - info.start_time;
    // TODO make key-val-list response
    free_request(c->request);

    return OK;
}


static int keys_handler(struct client *c) {

    struct key_command *cmd = c->request->command->kcommand;

    List *keys = trie_prefix_find(c->db->data, (const char *) cmd->key);

    struct response *response = make_list_response(keys, NULL, F_NOFLAG);

    struct buffer *buffer = buffer_init(response->lcontent->header->size);
    pack_response(buffer, response);

    set_reply(c, buffer, -1);

    list_release(keys, 1);
    free_response(response);
    free_request(c->request);

    return OK;
}


static bool compare_ttl(void *arg1, void *arg2) {

    uint64_t now = time(NULL);

    /* cast to cluster_node */
    const struct node_data *n1 = ((struct expiring_key *) arg1)->nd;
    const struct node_data *n2 = ((struct expiring_key *) arg2)->nd;

    uint64_t delta_l1 = (n1->ctime + n1->ttl) - now;
    uint64_t delta_l2 = (n2->ctime + n2->ttl) - now;

    return delta_l1 <= delta_l2;
}

/* Private function, insert or update values into the the trie database,
   updating, if any present, expiring keys vector */
static void put_data_into_trie(struct database *db,
        struct keyval_command *cmd) {

    int16_t ttl = cmd->ttl != 0 ? cmd->ttl : -NOTTL;

    /*
     * TODO refactor TTL insertion, for now it does not support expiration of
     * keys below a given prefix
     */
    if (cmd->is_prefix == 1) {
        trie_prefix_set(db->data, (const char *) cmd->key, cmd->val, ttl);
    } else {
        struct node_data *nd =
            trie_insert(db->data, (const char *) cmd->key, cmd->val);

        bool has_ttl = nd->ttl == -NOTTL ? false : true;

        // Update expiring keys if ttl != -NOTTL and sort it
        if (ttl != -NOTTL) {

            nd->ttl = cmd->ttl;

            /*
             * It's a new TTL, so we update creation_time to now in order to
             * calculate the effective expiration of the key
             */
            nd->ctime = nd->latime = (uint64_t) time(NULL);

            // Create a data strucuture to handle expiration
            struct expiring_key *ek = tmalloc(sizeof(*ek));
            ek->nd = nd;
            ek->key = tstrdup((const char *) cmd->key);
            ek->data_ptr = db->data;

            /*
             * Add the node data to the expiring keys only if it wasn't already
             * in, otherwise nothing should change cause the expiring keys
             * already got a pointer to the node data, which will now have an
             * updated TTL value
             */
            if (!has_ttl)
                vector_append(tritedb.expiring_keys, ek);

            /*
             * Quicksort in O(nlogn) if there's more than one element in the
             * vector
             */
            vector_qsort(tritedb.expiring_keys,
                    compare_ttl, sizeof(struct expiring_key));
        }

        // Update total counter of keys
        tritedb.keyspace_size++;
    }
}


static int put_handler(struct client *c) {

    struct request *request = c->request;

    if (request->reqtype == SINGLE_REQUEST) {

        struct keyval_command *cmd = request->command->kvcommand;

        // Insert data into the trie
        put_data_into_trie(c->db, cmd);

    } else {

        struct bulk_command *bcmd = request->bulk_command;

        // Apply insertion for each command
        for (uint32_t i = 0; i < bcmd->ncommands; i++)
            put_data_into_trie(c->db, bcmd->commands[i]->kvcommand);
    }

    // For now just a single response
    set_ack_reply(c, OK, NULL, F_NOFLAG);

    free_request(c->request);

    return OK;
}


static int get_handler(struct client *c) {

    struct key_command *cmd = c->request->command->kcommand;
    void *val = NULL;

    // Test for the presence of the key in the trie structure
    bool found = trie_find(c->db->data, (const char *) cmd->key, &val);

    if (found == false || val == NULL) {
        set_ack_reply(c, NOK, NULL, F_NOFLAG);
    } else {

        struct node_data *nd = val;

        // If the key results expired, remove it instead of returning it
        int64_t now = time(NULL);
        int64_t delta = (nd->ctime + nd->ttl) - now;

        if (nd->ttl != -NOTTL && delta <= 0) {
            trie_delete(c->db->data, (const char *) cmd->key);
            set_ack_reply(c, NOK, NULL, F_NOFLAG);
            // Update total keyspace counter
            tritedb.keyspace_size--;
        } else {

            // Update the last access time
            nd->latime = time(NULL);

            // and return it
            struct response *response =
                make_data_response(nd->data, NULL, F_NOFLAG);

            struct buffer *buffer =
                buffer_init(response->dcontent->header->size);

            pack_response(buffer, response);

            set_reply(c, buffer, -1);
            free_response(response);
        }
    }

    free_request(c->request);

    return OK;
}


static int ttl_handler(struct client *c) {

    struct key_command *cmd = c->request->command->kcommand;
    void *val = NULL;

    // Check for key presence in the trie structure
    bool found = trie_find(c->db->data, (const char *) cmd->key, &val);

    if (found == false || val == NULL) {
        set_ack_reply(c, NOK, NULL, F_NOFLAG);
    } else {
        struct node_data *nd = val;
        bool has_ttl = nd->ttl == -NOTTL ? false : true;
        nd->ttl = cmd->ttl;

        /*
         * It's a new TTL, so we update creation_time to now in order to
         * calculate the effective expiration of the key
         */
        nd->ctime = nd->latime = (uint64_t) time(NULL);
        struct expiring_key *ek = tmalloc(sizeof(*ek));
        ek->nd = nd;
        ek->key = tstrdup((const char *) cmd->key);
        ek->data_ptr = c->db->data;

        /*
         * Push into the expiring keys list and merge sort it shortly after,
         * this way we have a mostly updated list of expiring keys at each
         * insert, making it simpler and more efficient to cycle through them
         * and remove it later.
         */
        if (!has_ttl)
            vector_append(tritedb.expiring_keys, ek);

        vector_qsort(tritedb.expiring_keys,
                compare_ttl, sizeof(struct expiring_key));

        set_ack_reply(c, OK, NULL, F_NOFLAG);
    }

    free_request(c->request);

    return OK;
}


static int del_handler(struct client *c) {

    int code = OK;
    struct key_list_command *cmd = c->request->command->klcommand;
    bool found = false;

    // Flush all data in case of no prefixes passed
    if (cmd->len == 0) {
        trie_node_free(c->db->data->root, &c->db->data->size);
        // Update total keyspace counter
        tritedb.keyspace_size--;
    } else {
        size_t currsize = 0;
        for (int i = 0; i < cmd->len; i++) {

            /*
             * For each key in the keys array, check for presence and try to
             * remove it, if the `is_prefix` flag is a set the key will be
             * treated as a prefix wildcard (*) and we'll remove all keys below
             * it in the trie
             */
            if (cmd->keys[i]->is_prefix == 1) {

                currsize = c->db->data->size;

                /*
                 * We are dealing with a wildcard, so we apply the deletion to
                 * all keys below the wildcard
                 */
                trie_prefix_delete(c->db->data,
                        (const char *) cmd->keys[i]->key);

                // Update total keyspace counter
                tritedb.keyspace_size -= currsize - c->db->data->size;
            } else {
                found = trie_delete(c->db->data,
                        (const char *) cmd->keys[i]->key);
                if (found == false)
                    code = NOK;

                // Update total keyspace counter
                tritedb.keyspace_size--;
            }
        }
    }

    set_ack_reply(c, code, NULL, F_NOFLAG);
    free_request(c->request);

    return OK;
}

/*
 * Increment an integer value by 1. If the string value doesn't contain a
 * proper integer return a NOK.
 *
 * XXX check for bounds
 */
static int inc_handler(struct client *c) {

    int code = OK, n = 0;
    struct key_list_command *inc = c->request->command->klcommand;
    bool found = false;
    void *val = NULL;

    for (int i = 0; i < inc->len; i++) {

        if (inc->keys[i]->is_prefix == 1) {
            trie_prefix_inc(c->db->data, (const char *) inc->keys[i]->key);
        } else {

            /*
             * For each key in the keys array, check for presence and increment
             * it by one
             */
            found = trie_find(c->db->data,
                    (const char *) inc->keys[i]->key, &val);

            if (found == false || !val) {
                code = NOK;
            } else {
                struct node_data *nd = val;
                if (!is_integer(nd->data)) {
                    code = NOK;
                } else {
                    n = parse_int(nd->data);
                    ++n;
                    /*
                     * Check for realloc if the new value is "larger" then
                     * previous
                     */
                    char tmp[number_len(n)];  // max size in bytes
                    sprintf(tmp, "%d", n);  // XXX Unsafe
                    size_t len = strlen(tmp);
                    nd->data = trealloc(nd->data, len + 1);
                    strncpy(nd->data, tmp, len + 1);
                }
            }
        }
    }

    set_ack_reply(c, code, NULL, F_NOFLAG);
    free_request(c->request);

    return OK;
}

/*
 * Decrement an integer value by 1. If the string value doesn't contain a
 * proper integer return a NOK.
 *
 * XXX check for bounds
 */
static int dec_handler(struct client *c) {

    int code = OK, n = 0;
    struct key_list_command *dec = c->request->command->klcommand;
    bool found = false;
    void *val = NULL;

    for (int i = 0; i < dec->len; i++) {

        if (dec->keys[i]->is_prefix) {
            trie_prefix_dec(c->db->data, (const char *) dec->keys[i]->key);
        } else {

            /*
             * For each key in the keys array, check for presence and increment
             * it by one
             */
            found = trie_find(c->db->data,
                    (const char *) dec->keys[i]->key, &val);
            if (found == false || !val) {
                code = NOK;
            } else {
                struct node_data *nd = val;
                if (!is_integer(nd->data)) {
                    code = NOK;
                } else {
                    n = parse_int(nd->data);
                    --n;

                    /*
                     * Check for realloc if the new value is "smaller" then
                     * previous
                     */
                    char tmp[number_len(n)];
                    sprintf(tmp, "%d", n);
                    size_t len = strlen(tmp);
                    nd->data = trealloc(nd->data, len + 1);
                    strncpy(nd->data, tmp, len + 1);
                }
            }
        }
    }

    set_ack_reply(c, code, NULL, F_NOFLAG);
    free_request(c->request);

    return OK;
}

/* Get the current selected DB of the requesting client */
static int db_handler(struct client *c) {

    struct response *response =
        make_data_response((uint8_t *) c->db->name, NULL, F_NOFLAG);

    struct buffer *buffer = buffer_init(response->dcontent->header->size);
    pack_response(buffer, response);

    set_reply(c, buffer, -1);
    free_response(response);

    free_request(c->request);

    return OK;
}

/* Set the current selected namespace for the connected client. */
static int use_handler(struct client *c) {

    struct key_command *cmd = c->request->command->kcommand;

    /* Check for presence first */
    struct database *database =
        hashtable_get(tritedb.dbs, (const char *) cmd->key);

    /*
     * It doesn't exist, we create a new database with the given name,
     * otherwise just assign it to the current db of the client
     */
    if (!database) {
        // TODO check for OOM
        database = tmalloc(sizeof(*database));
        database->name = tstrdup((const char *) cmd->key);
        database->data = trie_create();
        // Add it to the databases table
        hashtable_put(tritedb.dbs, tstrdup(database->name), database);
        c->db = database;
    } else {
        c->db = database;
    }

    set_ack_reply(c, OK, NULL, F_NOFLAG);

    free_request(c->request);

    return OK;
}


static int count_handler(struct client *c) {

    int count = 0;
    struct key_command *cnt = c->request->command->kcommand;

    /*
     * Get the size of each key below the requested one, glob operation or the
     * entire trie size in case of NULL key
     */
    count = !cnt->key ? c->db->data->size :
        trie_prefix_count(c->db->data, (const char *) cnt->key);

    struct response *res = make_valuecontent_response(count, NULL, F_NOFLAG);

    struct buffer *b = buffer_init(res->vcontent->header->size);
    pack_response(b, res);
    set_reply(c, b, -1);
    free_response(res);

    free_request(c->request);

    return OK;
}


static int cluster_join_handler(struct client *c) {

    struct keyval_command *command = c->request->command->kvcommand;

    /* Only other nodes are enabled to send this request */
    if (!(command->header->flags & F_FROMNODEREQUEST))
        goto exit;

    /* UUID for the transaction, only for cluster operations */
    char uuid[UUID_LEN];
    generate_uuid(uuid);
    // TODO add it to the global map

    if (command->header->flags & F_JOINREQUEST) {

        // Send here the list of the other cluster members
        List *members = list_create(keyval_list_free);
        List *cluster_nodes = tritedb.cluster->nodes;

        for (struct list_node *ln = cluster_nodes->head; ln; ln = ln->next) {

            struct cluster_node *curr_node = ln->data;

            if (curr_node->self)
                continue;

            struct keyval *kv = tmalloc(sizeof(*kv));
            kv->keysize = strlen(curr_node->host);
            kv->key = (uint8_t *) tstrdup(curr_node->host);
            kv->valsize = strlen(curr_node->port);
            kv->val = (uint8_t *) tstrdup(curr_node->port);

            list_push(members, kv);
        }

        if (list_size(members) > 0) {
            struct response *response = make_kvlist_response(members,
                    (const uint8_t *) uuid, F_FROMNODERESPONSE | F_BULKREQUEST);

            struct buffer *buffer =
                buffer_init(response->kvlcontent->header->size);

            pack_response(buffer, response);

            set_reply(c, buffer, -1);

            free_response(response);

        } else {
            set_ack_reply(c, OK, (const uint8_t *) uuid, F_FROMNODERESPONSE);
        }

        list_release(members, 0);

    } else {
        set_ack_reply(c, OK, (const uint8_t *) uuid, F_FROMNODERESPONSE);
    }

    /*
     * Add new node to the hashring of this instance, key field of the command
     * structure should carry the host+port string joined together
     */
    cluster_add_new_node(tritedb.cluster, c,
            (const char *) command->key, (const char *) command->val, false);

    tdebug("[JOIN] Added %s:%s", command->key, command->val);

    free_request(c->request);

    return OK;

exit:

    free_request(c->request);

    return -1;
}


static int cluster_members_handler(struct client *c) {

    struct kvlist_content *content = c->response->kvlcontent;

    /* Only other nodes are enabled to send this response */
    if (!(content->header->flags & F_FROMNODERESPONSE) || content->len == 0)
        return -1;

    struct keyval *pair = content->pairs[0];

    // Connect to the first node target
    int port = atoi((const char *) pair->val) + 10000;

    int fd = open_connection((const char *) pair->key, port);

    // Create a new client for it
    if (set_nonblocking(fd) < 0)
        perror("set_nonblocking: ");

    if (set_tcp_nodelay(fd) < 0)
        perror("set_tcp_nodelay: ");

    struct client *new_node = tmalloc(sizeof(*new_node));

    new_node->ctype = NODE;
    new_node->addr = tstrdup((const char *) pair->key);
    new_node->fd = fd;
    new_node->last_action_time = time(NULL);
    new_node->ctx_handler = reply_handler;
    new_node->reply = NULL;
    new_node->request = NULL;
    new_node->response = NULL;
    new_node->db = hashtable_get(tritedb.dbs, "db0");

    generate_uuid((char *) new_node->uuid);

    if (add_epoll(tritedb.epollfd, fd, EPOLLOUT, new_node) < 0)
        perror("epoll_add: ");

    /*
     * Add new node to the hashring of this instance, key field of the
     * command structure should carry the host+port string joined together
     */
    cluster_add_new_node(tritedb.cluster, new_node,
            (const char *) pair->key, (const char *) pair->val, false);

    /* Track the new connected node */
    hashtable_put(tritedb.nodes, new_node->uuid, new_node);

    tdebug("[MEMBER] Added %s:%s", pair->key, pair->val);

    // Send CLUSTER_JOIN request
    // XXX Obnoxious
    struct request *request =
        make_join_request(conf->hostname, conf->port, F_FROMNODEREQUEST);

    struct buffer *buffer =
        buffer_init(request->command->kvcommand->header->size);

    pack_request(buffer, request, KEY_VAL_COMMAND);

    set_reply(new_node, buffer, -1);

    free_request(request);

    // Update informations
    info.nconnections++;
    info.nnodes++;

    // XXX check
    tfree(pair->key);
    tfree(pair->val);
    tfree(pair);

    /* Push other awaiting cluster members into a connection queue */
    if (content->len > 1)
        for (int i = 1; i < content->len; i++)
            queue_push(tritedb.pending_members, content->pairs[i]);

    // clean out partial structure
    tfree(content->header);
    tfree(content->pairs);
    tfree(content);
    tfree(c->response);

    return OK;
}


/**************************************/
/*          SERVER_HANDLERS           */
/**************************************/


static int route_command(struct request *request,
        struct buffer *buffer, struct client *client) {

    int ret = 0;

    if (request->reqtype == SINGLE_REQUEST) {

        int16_t hashval = -1;

        switch (request->command->cmdtype) {
            case KEY_COMMAND:
            case KEY_VAL_COMMAND:

                /*
                 * Compute a CRC32(key) % RING_POINTS, we get an index for a
                 * position in the consistent hash ring and retrieve the node
                 * in charge to handle the request
                 */
                hashval = hash((const char *) request->command->kcommand->key);

                struct cluster_node *node =
                    cluster_get_node(tritedb.cluster, hashval);

                /*
                 * Sent out all bytes. TODO: raise a EPOLLOUT event and handle
                 * the operation in a cleaner way
                 */
                ssize_t sent;
                if ((sendall(node->link->fd, buffer->data,
                                buffer->size, &sent)) < 0) {
                    perror("send(2): can't write on socket descriptor");
                    ret = -1;
                }

                // Update information stats
                info.noutputbytes += sent;

                /*
                 * Update cluster transactions in order to know who to answer to
                 * when the other node will reply back with result
                 */
                char transaction_id[UUID_LEN];
                generate_uuid(transaction_id);

                /* Track the transaction */
                hashtable_put(tritedb.transactions, transaction_id, client);

                break;
            default:
                tdebug("Not implemented yet");
                break;
        }

    } else {
    }

    return ret;
}

/* Handle incoming requests, after being accepted or after a reply */
static int request_handler(struct client *client) {

    int clientfd = client->fd;

    /*
     * struct buffer to initialize the ring buffer, used to handle input from
     * client
     */
    uint8_t *buffer = tmalloc(conf->max_request_size);

    /*
     * Ringbuffer pointer struct, helpful to handle different and unknown
     * size of chunks of data which can result in partially formed packets or
     * overlapping as well
     */
    Ringbuffer *rbuf = ringbuf_create(buffer, conf->max_request_size);

    uint8_t opcode = 0;
    int rc = 0;

    /*
     * We must read all incoming bytes till an entire packet is received. This
     * is achieved by using a standardized protocol, which send the size of the
     * complete packet as the first 4 bytes. By knowing it we know if the
     * packet is ready to be deserialized and used.
     */
    struct buffer *b = recv_packet(clientfd, rbuf, &opcode, &rc);

    /*
     * Looks like we got a client disconnection.
     * TODO: Set a error_handler for ERRMAXREQSIZE instead of dropping client
     *       connection, explicitly returning an informative error code to the
     *       client connected.
     */
    if (rc == -ERRCLIENTDC || rc == -ERRMAXREQSIZE) {
        ringbuf_release(rbuf);
        tfree(buffer);
        goto errclient;
    }

    /*
     * If not correct packet received, we must free ringbuffer and reset the
     * handler to the request again, setting EPOLL to EPOLLIN
     */
    if (!b)
        goto freebuf;

    // Update information stats
    info.ninputbytes += b->size;

    /*
     * Currently we have a stream of bytes, we want to unpack them into a
     * request structure or a response structure
     */
    if (rc == 1 || opcode == ACK) {

        struct response *response = unpack_response(b);

        /*
         * If the packet couldn't be unpacked (e.g. we're OOM) we close the
         * connection and release the client
         */
        if (!response)
            goto errclient;

        /* Link the response to the client that sent it */
        client->response = response;

    } else {

        struct request *request = unpack_request(b);

        /*
         * If the packet couldn't be unpacked (e.g. we're OOM) we close the
         * connection and release the client
         */
        if (!request)
            goto errclient;

        /*
         * Link the correct structure to the client, according to the packet type
         * received, this time it's a request
         */
        client->request = request;

        /*
         * If the mode is cluster and the requesting client is not a node nor a
         * server but another node in the cluster, we should route the command to
         * the correct node before handling it.
         */
        if (client->ctype == CLIENT && conf->mode == CLUSTER)
            route_command(request, b, client);
    }

    /* No more need of the byte buffer from now on */
    buffer_destroy(b);

    /* Free ring buffer as we alredy have all needed informations in memory */
    ringbuf_release(rbuf);

    tfree(buffer);

    // Update client last action time
    client->last_action_time = (uint64_t) time(NULL);

    int executed = 0;
    int err = 0;

    // Loop through commands_hashmap array to find the correct handler
    for (int i = 0; i < COMMAND_COUNT; i++) {
        if (commands_map[i].ctype == opcode) {
            err = commands_map[i].handler(client);
            executed = 1;
        }
    }

    // Record request on the counter
    info.nrequests++;

    // If no handler is found, it must be an error case
    if (executed == 0 || err == 5)
        goto reset;

    /*
     * A disconnection happened, we close the handler, the file descriptor
     * have been already removed from the event loop
     */
    if (err == -1)
        goto exit;

    // Set reply handler as the current context handler
    client->ctx_handler = reply_handler;

    /*
     * Reset handler to request_handler in order to read new incoming data and
     * EPOLL event for read fds
     */
    mod_epoll(tritedb.epollfd, clientfd, EPOLLOUT, client);

exit:

    return 0;

freebuf:

    ringbuf_release(rbuf);
    tfree(buffer);

reset:

    client->ctx_handler = request_handler;
    mod_epoll(tritedb.epollfd, clientfd, EPOLLIN, client);
    return 0;

errclient:

    terror("Dropping client on %s", client->addr);
    shutdown(client->fd, 0);
    close(client->fd);
    hashtable_del(tritedb.clients, client->uuid);
    info.nclients--;
    return -1;
}


/*
 * Handle reply state, after a request/response has been processed in
 * request_handler routine. Just send out all bytes stored in the reply buffer
 * to the reply file descriptor, which can be either a connected client or a
 * tritedb node connected to the bus port.
 */
static int reply_handler(struct client *client) {

    int ret = 0;
    if (!client->reply)
        return ret;

    struct reply *reply = client->reply;
    ssize_t sent;

    if ((sendall(reply->fd, reply->payload->data,
                    reply->payload->size, &sent)) < 0) {
        perror("send(2): can't write on socket descriptor");
        ret = -1;
    }

    // Update information stats
    info.noutputbytes += sent;

    free_reply(client->reply);
    client->reply = NULL;

    /* Set up EPOLL event on EPOLLIN to read fds */
    client->ctx_handler = request_handler;
    mod_epoll(tritedb.epollfd, client->fd, EPOLLIN, client);

    return ret;
}

/*
 * Accept a new incoming connection assigning ip address and socket descriptor
 * to the connection structure pointer passed as argument
 */
static int accept_new_client(int fd, struct connection *conn) {

    if (!conn)
        return -1;

    /* Accept the connection */
    int clientsock = accept_connection(fd);

    /* Abort if not accepted */
    if (clientsock == -1)
        return -1;

    /* Just some informations retrieval of the new accepted client connection */
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);

    if (getpeername(clientsock, (struct sockaddr *) &addr, &addrlen) < 0)
        return -1;

    char ip_buff[INET_ADDRSTRLEN + 1];
    if (inet_ntop(AF_INET, &addr.sin_addr, ip_buff, sizeof(ip_buff)) == NULL)
        return -1;

    struct sockaddr_in sin;
    socklen_t sinlen = sizeof(sin);

    if (getsockname(fd, (struct sockaddr *) &sin, &sinlen) < 0)
        return -1;

    conn->fd = clientsock;
    strcpy(conn->ip, ip_buff);

    return 0;
}

/*
 * Handle new connection, create a a fresh new struct client structure and link
 * it to the fd, ready to be set in EPOLLIN event
 */
static int accept_handler(struct client *server) {

    struct connection conn;

    accept_new_client(server->fd, &conn);

    /* Create a client structure to handle his context connection */
    struct client *client = tmalloc(sizeof(struct client));
    if (!client)
        oom("creating client during accept");

    /* Generate random uuid */
    uuid_t binuuid;
    uuid_generate_random(binuuid);
    uuid_unparse(binuuid, (char *) client->uuid);

    /* Populate client structure */
    client->ctype = CLIENT;
    client->addr = tstrdup(conn.ip);
    client->fd = conn.fd;
    client->ctx_handler = request_handler;

    /* Record last action as of now */
    client->last_action_time = (uint64_t) time(NULL);

    client->reply = NULL;

    /* Set the default db for the current user */
    client->db = hashtable_get(tritedb.dbs, "db0");

    /* Add it to the db instance */
    hashtable_put(tritedb.clients, client->uuid, client);

    /* Add it to the epoll loop */
    add_epoll(tritedb.epollfd, conn.fd, EPOLLIN, client);

    /* Rearm server fd to accept new connections */
    mod_epoll(tritedb.epollfd, server->fd, EPOLLIN, server);

    /* Record the new client connected */
    info.nclients++;
    info.nconnections++;

    return 0;
}

/*
 * Accept a tritedb instance connecting from a (at least logical) separate node
 * by setting the client connected as type NODE
 */
static int accept_node_handler(struct client *bus_server) {

    struct connection conn;

    accept_new_client(bus_server->fd, &conn);

    /* Create a client structure to handle his context connection */
    struct client *new_node = tmalloc(sizeof(struct client));
    if (!new_node)
        oom("creating new_node during accept");

    /* Generate random uuid */
    uuid_t binuuid;
    uuid_generate_random(binuuid);
    uuid_unparse(binuuid, (char *) new_node->uuid);

    /* Populate new_node structure */
    new_node->ctype = NODE;
    new_node->addr = tstrdup(conn.ip);
    new_node->fd = conn.fd;
    new_node->ctx_handler = request_handler;

    /* Record last action as of now */
    new_node->last_action_time = (uint64_t) time(NULL);

    new_node->reply = NULL;

    /* Set the default db for the current user */
    new_node->db = hashtable_get(tritedb.dbs, "db0");

    /* Add it to the db instance */
    hashtable_put(tritedb.nodes, new_node->uuid, new_node);

    /* Add it to the epoll loop */
    add_epoll(tritedb.epollfd, conn.fd, EPOLLIN, new_node);

    /* Rearm server fd to accept new connections */
    mod_epoll(tritedb.epollfd, bus_server->fd, EPOLLIN, bus_server);

    /* Record the new node connected */
    info.nnodes++;
    info.nconnections++;

    return 0;

}


static void free_expiring_keys(Vector *ekeys) {

    if (!ekeys)
        return;

    struct expiring_key *ek = NULL;

    for (int i = 0; i < ekeys->size; i++) {

        ek = vector_get(ekeys, i);

        if (!ek)
            continue;

        if (ek->key)
            tfree((char *) ek->key);

        tfree(ek);
    }

    // free vector structure pointer
    tfree(ekeys->items);
    tfree(ekeys);
}

/*
 * Cycle through sorted list of expiring keys and remove those which are
 * elegible
 */
static void expire_keys(void) {

    if (vector_size(tritedb.expiring_keys) == 0)
        return;

    int64_t now = (int64_t) time(NULL);
    int64_t delta = 0LL;
    struct expiring_key *ek = NULL;

    for (int i = 0; i < vector_size(tritedb.expiring_keys); i++) {

        ek = vector_get(tritedb.expiring_keys, i);
        delta = (ek->nd->ctime + ek->nd->ttl) - now;

        if (delta > 0)
            break;

        /*
         * ek->data_ptr points to the trie of the client which stores the given
         * key
         */
        trie_delete(ek->data_ptr, ek->key);

        vector_delete(tritedb.expiring_keys, i);

        // Update total keyspace counter
        tritedb.keyspace_size--;

        tdebug("EXPIRING %s", ek->key);

        tfree((char *) ek->key);
        tfree(ek);
        ek = NULL;
    }
}

/* Print and log some basic stats */
static inline void log_stats(void) {
    info.uptime = time(NULL) - info.start_time;
    const char *uptime = time_to_string(info.uptime);
    const char *memory = memory_to_string(memory_used());
    tdebug("Connected clients: %d total connections: %d "
            "requests: %d dbs: %ld keys: %ld memory usage: %s  uptime: %s",
            info.nclients, info.nconnections, info.nrequests, tritedb.dbs->size,
            tritedb.keyspace_size, memory, uptime);
    tfree((char *) uptime);
    tfree((char *) memory);
}

/*
 * Temporary auxiliary function till the network::epoll_loop is ready, add a
 * periodic task to the epoll loop
 */
static int add_cron_task(int epollfd, struct itimerspec timervalue) {

    int timerfd = timerfd_create(CLOCK_MONOTONIC, 0);

    if (timerfd_settime(timerfd, 0, &timervalue, NULL) < 0)
        perror("timerfd_settime");

    // Add the timer to the event loop
    struct epoll_event ev;
    ev.data.fd = timerfd;
    ev.events = EPOLLIN;

    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, timerfd, &ev) < 0) {
        perror("epoll_ctl(2): EPOLLIN");
        return -1;
    }

    return timerfd;
}

/*
 * Main worker function, his responsibility is to wait on events on a shared
 * EPOLL fd, use the same way for clients or peer to distribute messages
 */
static void run_server(void) {

    struct epoll_event *evs = tmalloc(sizeof(*evs) * MAX_EVENTS);

    if (!evs)
        oom("allocating events");

    int timeout = conf->epoll_timeout;
    int events = 0;

    struct itimerspec timervalue;

    memset(&timervalue, 0x00, sizeof(timervalue));

    timervalue.it_value.tv_sec = 0;
    timervalue.it_value.tv_nsec = TTL_CHECK_INTERVAL;
    timervalue.it_interval.tv_sec = 0;
    timervalue.it_interval.tv_nsec = TTL_CHECK_INTERVAL;

    // add expiration keys cron task
    int exptimerfd = add_cron_task(tritedb.epollfd, timervalue);

    int statstimerfd = -1;
    if (conf->loglevel == DEBUG) {
        struct itimerspec st_timervalue;

        memset(&timervalue, 0x00, sizeof(st_timervalue));

        st_timervalue.it_value.tv_sec = STATS_PRINT_INTERVAL;
        st_timervalue.it_value.tv_nsec = 0;
        st_timervalue.it_interval.tv_sec = STATS_PRINT_INTERVAL;
        st_timervalue.it_interval.tv_nsec = 0;

        statstimerfd = add_cron_task(tritedb.epollfd, st_timervalue);
    }

    long int timers = 0;

    while (1) {

        events = epoll_wait(tritedb.epollfd, evs, MAX_EVENTS, timeout);

        if (events < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }

        for (int i = 0; i < events; i++) {

            /* Check for errors first */
            if ((evs[i].events & EPOLLERR) ||
                    (evs[i].events & EPOLLHUP) ||
                    (!(evs[i].events & EPOLLIN) && !(evs[i].events & EPOLLOUT))) {

                /*
                 * An error has occured on this fd, or the socket is not ready
                 * for reading
                 */
                struct client *client = evs[i].data.ptr;

                terror("Dropping client on %s: event polling error %s",
                        client->addr, strerror(errno));

                /*
                 * Clean out from global tables, from clients or from nodes
                 * based on the client type
                 * TODO: unify with if below
                 */
                if (client->ctype == NODE)
                    info.nnodes--;
                else
                    info.nclients--;

                hashtable_del(client->ctype == NODE ?
                        tritedb.nodes : tritedb.clients, client->uuid);

                close(evs[i].data.fd);

                continue;

            } else if (evs[i].data.fd == conf->run) {

                /* And quit event after that */
                eventfd_t val;
                eventfd_read(conf->run, &val);

                tdebug("Stopping epoll loop.");

                goto exit;

            } else if (exptimerfd != -1 && evs[i].data.fd == exptimerfd) {
                (void) read(evs[i].data.fd, &timers, 8);
                // Check for keys about to expire out
                expire_keys();
            } else if (statstimerfd != -1 && evs[i].data.fd == statstimerfd) {
                (void) read(evs[i].data.fd, &timers, 8);
                // Print stats about the server
                log_stats();
            } else {
                /* Finally handle the request according to its type */
                ((struct client *) evs[i].data.ptr)->ctx_handler(evs[i].data.ptr);
            }
        }
    }

exit:

    if (events <= 0 && conf->run != 1)
        perror("epoll_wait(2) error");

    tfree(evs);
}

/*
 * Main entry point for start listening on a socket and running an epoll event
 * loop his main responsibility is to pass incoming client connections
 * descriptor to workers thread.
 * In this case there's no threads and uses just a single thread with
 * multiplexing I/O, but it's trivial to transform the main run_server routine
 * into a thread worker and launch some more (e.g. one per CPU core).
 *
 * Accepts two mandatory string arguments, addr and port, in case of UNIX
 * domain socket, addr represents the path on the FS where the socket fd is
 * located, port will be ignored.
 */
int start_server(const char *addr,
        const char *port, struct seed_node *seednode) {

    /* Initialize SizigyDB server object */
    tritedb.clients = hashtable_create(hashtable_client_free);
    tritedb.nodes = hashtable_create(hashtable_client_free);
    tritedb.expiring_keys = vector_create();
    tritedb.dbs = hashtable_create(database_free);
    tritedb.keyspace_size = 0LL;
    // TODO add free cluster
    tritedb.cluster = &(struct cluster) { list_create(NULL) };
    tritedb.transactions = hashtable_create(hashtable_client_free);
    tritedb.pending_members = queue_create(keyval_queue_free);

    /* Create default database */
    struct database *default_db = tmalloc(sizeof(struct database));
    default_db->name = tstrdup("db0");
    default_db->data = trie_create();

    /* Add it to the global map */
    hashtable_put(tritedb.dbs, tstrdup(default_db->name), default_db);

    /* Initialize epollfd for server component */
    int epollfd = epoll_create1(0);

    if (epollfd == -1) {
        perror("epoll_create1");
        goto cleanup;
    }

    tritedb.epollfd = epollfd;

    /* Initialize the sockets, first the server one */
    int fd = make_listen(addr, port, conf->socket_family);

    /*
     * Add eventfd to the loop, this time only in LT in order to wake up all
     * threads
     */
    struct epoll_event ev;
    ev.data.fd = conf->run;
    ev.events = EPOLLIN;

    if (epoll_ctl(epollfd, EPOLL_CTL_ADD, conf->run, &ev) < 0)
        perror("epoll_ctl(2): add epollin");

    /*
     * Client structure for the server component, start in the ACCEPT state,
     * ready to accept new connections from client and handle commands.
     */
    struct client server = {
        .ctype = SERVER,
        .addr = addr,
        .fd = fd,
        .last_action_time = time(NULL),
        .ctx_handler = accept_handler,
        .reply = NULL,
        .request = NULL,
        .response = NULL,
        .db = NULL
    };

    generate_uuid((char *) server.uuid);

    /* Set socket in EPOLLIN flag mode, ready to read data */
    add_epoll(epollfd, fd, EPOLLIN, &server);

    /*
     * Add socket for bus communication if accepted by a seed node
     * TODO make it in another thread or better, crate a usable client
     * structure like if it was accepted as a new connection, cause actually
     * it crashes the server by having NULL ptr
     */
    if (seednode->fd > 0) {

        struct client *node = tmalloc(sizeof(*node));

        if (set_nonblocking(seednode->fd) < 0)
            perror("set_nonblocking: ");

        if (set_tcp_nodelay(seednode->fd) < 0)
            perror("set_tcp_nodelay: ");

        node->ctype = NODE;
        node->addr = tstrdup(seednode->addr);
        node->fd = seednode->fd;
        node->last_action_time = time(NULL);
        node->ctx_handler = reply_handler;
        node->reply = NULL;
        node->request = NULL;
        node->response = NULL;
        node->db = hashtable_get(tritedb.dbs, "db0");

        generate_uuid((char *) node->uuid);

        if (add_epoll(epollfd, seednode->fd, EPOLLOUT, node) < 0)
            perror("epoll_add: ");

        // Send CLUSTER_JOIN request
        // XXX Obnoxious
        struct request *request =
            make_join_request(addr, port, F_FROMNODEREQUEST | F_JOINREQUEST);

        struct buffer *buffer =
            buffer_init(request->command->kvcommand->header->size);

        pack_request(buffer, request, KEY_VAL_COMMAND);

        set_reply(node, buffer, -1);

        free_request(request);

        // Update informations
        info.nconnections++;
        info.nnodes++;

        // Add target node as cluster node
        cluster_add_new_node(tritedb.cluster, node,
                seednode->addr, seednode->port, false);

        /* Track the new connected node on the cluster */
        hashtable_put(tritedb.nodes, node->uuid, node);

        tdebug("Added %s:%s", seednode->addr, seednode->port);
    }

    /*
     * If it is run in CLUSTER mode add an additional descriptor and register
     * it to the event loop, ready to accept incoming connections from other
     * tritedb nodes and handle cluster commands.
     */
    if (conf->mode == CLUSTER) {

        struct client *bus_server = tmalloc(sizeof(*bus_server));

        /* Add 10k to the listening server port */
        int bport = atoi(port) + 10000;
        snprintf(tritedb.busport, number_len(bport), "%d", bport);

        /* The bus one for distribution */
        int bfd = make_listen(addr, tritedb.busport, INET);

        /* struct client structure for the bus server component */
        bus_server->ctype = SERVER;
        bus_server->addr = tstrdup(addr);
        bus_server->fd = bfd;
        bus_server->last_action_time = time(NULL);
        bus_server->ctx_handler = accept_node_handler;
        bus_server->reply = NULL;
        bus_server->request = NULL;
        bus_server->response = NULL;
        bus_server->db = NULL;

        /* Set bus socket in EPOLLIN too */
        add_epoll(tritedb.epollfd, bfd, EPOLLIN, bus_server);

        cluster_add_new_node(tritedb.cluster, bus_server, addr, port, true);

        /* Track the new connected node on the cluster */
        hashtable_put(tritedb.nodes, bus_server->uuid, bus_server);

        tdebug("Added %s:%s [self]", addr, port);
    }

    tinfo("TriteDB v%s", conf->version);

    if (conf->socket_family == UNIX)
        tinfo("Starting server on %s", addr);
    else
        tinfo("Starting server on %s:%s", addr, port);

    if (conf->mode == CLUSTER)
        tinfo("Opened bus port on %s:%s", addr, tritedb.busport);

    // Record start time
    info.start_time = time(NULL);

    // Start spinning the ferry-wheel!
    run_server();

cleanup:

    /* Free all resources allocated */
    hashtable_release(tritedb.nodes);
    hashtable_release(tritedb.clients);
    hashtable_release(tritedb.transactions);
    hashtable_release(tritedb.dbs);
    queue_release(tritedb.pending_members);
    list_release(tritedb.cluster->nodes, 1);
    free_expiring_keys(tritedb.expiring_keys);

    tdebug("Bye\n");
    return 0;
}
