/* BSD 2-Clause License
 *
 * Copyright (c) 2018, 2019 Andrea Giacomo Baldan All rights reserved.
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
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <uuid/uuid.h>
#include "list.h"
#include "pack.h"
#include "util.h"
#include "server.h"
#include "config.h"
#include "network.h"
#include "protocol.h"


/*
 * TCP server, based on I/O multiplexing but sharing I/O and work loads between
 * thread pools. The main thread have the exclusive responsibility of accepting
 * connections and pass them to IO threads.
 * From now on read and write operations for the connection will be handled by
 * a dedicated thread pool, which after every read will decode the bytearray
 * according to the protocol definition of each packet and finally pass the
 * resulting packet to the worker thread pool, where, according to the OPCODE
 * of the packet, the operation will be executed and the result will be
 * returned back to the IO thread that will write back to the client the
 * response packed into a bytestream.
 *
 *      MAIN              1...N              1...N
 *
 *     [EPOLL]         [IO EPOLL]         [WORK EPOLL]
 *  ACCEPT THREAD    IO THREAD POOL    WORKER THREAD POOL
 *  -------------    --------------    ------------------
 *        |                 |                  |
 *      ACCEPT              |                  |
 *        | --------------> |                  |
 *        |          READ AND DECODE           |
 *        |                 | ---------------> |
 *        |                 |                WORK
 *        |                 | <--------------- |
 *        |               WRITE                |
 *        |                 |                  |
 *      ACCEPT              |                  |
 *        | --------------> |                  |
 *
 * By tuning the number of IO threads and worker threads based on the number of
 * core of the host machine, it is possible to increase the number of served
 * concurrent requests per seconds.
 * The main Trie data strucure accessed on the worker thread pool is guarded by
 * a spinlock, and being generally fast operations it shouldn't suffer high
 * contentions by the threads and thus being really fast.
 */


/*
 * Guards the access to the main database structure, the trie underlying the
 * DB
 */
static pthread_spinlock_t spinlock;

/*
 * IO event strucuture, it's the main information that will be communicated
 * between threads, every request packet will be wrapped into an IO event and
 * passed to the work EPOLL, in order to be handled by the worker thread pool.
 * Then finally, after the execution of the command, it will be updated and
 * passed back to the IO epoll loop to be written back to the requesting client
 */
struct io_event {
    int epollfd;
    eventfd_t io_event;
    struct client *client;
    bstring reply;
    union triedb_request *payload;
};

/* Global information structure */
static struct informations info;

/* The main triedb instance */
static struct triedb triedb;

/*
 * Shared epoll object, contains the IO epoll and Worker epoll descriptors,
 * as well as the server descriptor and the timer fd for repeated routines.
 * Each thread will receive a copy of a pointer to this structure, to have
 * access to all file descriptor running the application
 */
struct epoll {
    int io_epollfd;
    int w_epollfd;
    int serverfd;
    int expirefd;
    int busfd;
};


static void init_info(void);
static void expire_keys(void);
static inline bool trie_node_destructor(struct trie_node *, bool);

/* Prototype for a command handler */
typedef int handler(struct io_event *);

/*
 * Command handler, each one have responsibility over a defined command
 * packet, each one will be called according to the opcode of every incoming
 * command request, on the worker threads.
 */
static int db_handler(struct io_event *);

static int put_handler(struct io_event *);

static int get_handler(struct io_event *);

static int del_handler(struct io_event *);

static int ttl_handler(struct io_event *);

static int inc_handler(struct io_event *);

static int dec_handler(struct io_event *);

static int cnt_handler(struct io_event *);

static int use_handler(struct io_event *);

static int keys_handler(struct io_event *);

static int ping_handler(struct io_event *);

static int quit_handler(struct io_event *);

static int info_handler(struct io_event *);

static int flush_handler(struct io_event *);

/* Command handler mapped usign their position paired with their type */
static handler *handlers[15] = {
    NULL,
    put_handler,
    get_handler,
    del_handler,
    ttl_handler,
    inc_handler,
    dec_handler,
    cnt_handler,
    use_handler,
    keys_handler,
    ping_handler,
    quit_handler,
    db_handler,
    info_handler,
    flush_handler
};

/* OK, NOK and RESERVED return codes, pre-packed ACK responses */
static bstring ack_replies[3];


/********************************/
/*      COMMAND HANDLERS        */
/********************************/

/* Get the current selected DB of the requesting client */
static int db_handler(struct io_event *event) {

    // TODO set DB response
    event->reply = bstring_new(event->client->db->name);

    return 0;
}


static int put_handler(struct io_event *event) {

    union triedb_request *packet = event->payload;
    struct client *c = event->client;

#if WORKERPOOLSIZE > 1
    pthread_spin_lock(&spinlock);
#endif

    if (packet->header.bits.prefix == 1) {
        database_prefix_set(c->db, (const char *) packet->put.key,
                            packet->put.val, packet->put.ttl);
    } else {
        database_insert(c->db, (const char *) packet->put.key,
                        packet->put.val, packet->put.ttl);
        // Update total counter of keys
        triedb.keyspace_size++;
    }

#if WORKERPOOLSIZE > 1
    pthread_spin_unlock(&spinlock);
#endif
    event->reply = ack_replies[OK];

    return 0;
}


static int get_handler(struct io_event *event) {

    union triedb_request *packet = event->payload;
    struct client *c = event->client;
    struct get_response *response = NULL;

    void *val = NULL;
    Vector *v = NULL;

    if (packet->get.header.bits.prefix == 0) {

        // Single value response

#if WORKERPOOLSIZE > 1
        pthread_spin_lock(&spinlock);
#endif
        // Test for the presence of the key in the trie structure
        bool found = database_search(c->db, (const char *) packet->get.key, &val);
#if WORKERPOOLSIZE > 1
        pthread_spin_unlock(&spinlock);
#endif

        if (found == false || val == NULL)
            goto nok;

        // XXX Lot of boilerplate, to be refactored

        struct db_item *item = val;

        /*
         * Check for TTL, in case of no ttl (-1) go on, otherwise the key is
         * flagged as expired and we delete it from the database, in a lazy
         * check behaviour
         */
        if (item->ttl != -1) {

            time_t now = time(NULL);
            time_t delta = (item->ctime + item->ttl) - now;

            if (delta <= 0) {

#if WORKERPOOLSIZE > 1
                pthread_spin_lock(&spinlock);
#endif
                // we're in the expired state
                database_remove(c->db, (const char *) packet->get.key);

                /*
                 * Linearly search for index of the key in the expiring_keys
                 * vector, binary search would be better here
                 */
                for (size_t i = 0; i < vector_size(triedb.expiring_keys); ++i) {
                    struct expiring_key *ek =
                        vector_get(triedb.expiring_keys, i);
                    if (ek->item == item) {
                        vector_delete(triedb.expiring_keys, i);
                        tfree((char *) ek->key);
                        tfree(ek);
                        break;
                    }
                }

                triedb.keyspace_size--;

#if WORKERPOOLSIZE > 1
                pthread_spin_unlock(&spinlock);
#endif

                // Finally return a NOK
                event->reply = ack_replies[NOK];

                return 0;
            }
        }

        struct tuple t = {
            .ttl = item->ttl,
            .keylen = strlen((const char *) packet->get.key),
            .key = packet->get.key,
            .val = item->data
        };

        response = get_response(packet->get.header.byte, &t);

    } else {

        /*
         * Multiple values response
         * In this case we have to check for TTL of each returned item from
         * the query and discard all those who have expired their time which
         * are not already being removed by the system itself through timerfd
         * routine on worker threads
         */

#if WORKERPOOLSIZE > 1
        pthread_spin_lock(&spinlock);
#endif

        v = database_prefix_search(c->db, (const char *) packet->get.key);

#if WORKERPOOLSIZE > 1
        pthread_spin_unlock(&spinlock);
#endif

        time_t now = time(NULL);
        time_t delta = 0;

        struct kv_obj *cur = NULL;
        struct db_item *item = NULL;
        for (int i = 0; i < vector_size(v); ++i) {
            cur = vector_get(v, i);
            item = (struct db_item *) cur->data;

            if (item->ttl != -1) {

                delta = (item->ctime + item->ttl) - now;

                if (delta <= 0) {

#if WORKERPOOLSIZE > 1
                    pthread_spin_lock(&spinlock);
#endif
                    // we're in the expired state
                    database_remove(c->db, (const char *) packet->get.key);

                    /*
                     * Linearly search for index of the key in the expiring_keys
                     * vector, binary search would be better here
                     */
                    for (size_t i = 0; i < vector_size(triedb.expiring_keys); ++i) {
                        struct expiring_key *ek =
                            vector_get(triedb.expiring_keys, i);
                        if (ek->item == item) {
                            vector_delete(triedb.expiring_keys, i);
                            tfree((char *) ek->key);
                            tfree(ek);
                            break;
                        }
                    }

                    triedb.keyspace_size--;

#if WORKERPOOLSIZE > 1
                    pthread_spin_unlock(&spinlock);
#endif

                    vector_delete(v, i);
                }
            }
        }

        /*
         * Prefix request can return either a populated vector with at least
         * one match, or a NULL pointer, in this case we'd change our response
         * to a simple NOK
         */
        if (v)
            response = get_response(packet->get.header.byte, v);
        else {
            event->reply = ack_replies[NOK];
            return 0;
        }
    }

    // XXX Lot of boilerplate, to be refactored

    union triedb_response r = { .get_res = *response };

    event->reply = pack_response(&r, packet->get.header.bits.opcode);

    // TODO destroy response
    if (v) {
        struct kv_obj *current = NULL;
        for (int i = 0; i < vector_size(v); ++i) {
            current = vector_get(v, i);
            tfree((void *) current->key);
            tfree(current);
        }
        tfree(v->items);
        tfree(v);
    }

    if (r.get_res.header.bits.prefix == 1)
        tfree(response->tuples);
    tfree(response);

    return 0;

nok:

    event->reply = ack_replies[NOK];

    return 0;

}


static int del_handler(struct io_event *event) {

    union triedb_request *packet = event->payload;
    struct client *c = event->client;

    size_t currsize = 0;

    /*
     * For each key in the keys array, check for presence and try to remove it,
     * if the `is_prefix` flag is a set the key will be treated as a prefix
     * wildcard (*) and we'll remove all keys below it in the trie
     */
    if (packet->get.header.bits.prefix == 1) {

        currsize = database_size(c->db);

#if WORKERPOOLSIZE > 1
        pthread_spin_lock(&spinlock);
#endif
        /*
         * We are dealing with a wildcard, so we apply the deletion
         * to all keys below the wildcard
         */
        database_prefix_remove(c->db, (const char *) packet->get.key);

#if WORKERPOOLSIZE > 1
        pthread_spin_unlock(&spinlock);
#endif

        // Update total keyspace counter
        triedb.keyspace_size -= currsize - database_size(c->db);

        event->reply = ack_replies[OK];

    } else {

#if WORKERPOOLSIZE > 1
        pthread_spin_lock(&spinlock);
#endif
        bool found = database_remove(c->db, (const char *) packet->get.key);
#if WORKERPOOLSIZE > 1
        pthread_spin_unlock(&spinlock);
#endif
        if (found == false)
            event->reply = ack_replies[NOK];
        else {
            // Update total keyspace counter
            triedb.keyspace_size--;
            event->reply = ack_replies[OK];
        }
    }

    return 0;
}

/*
 * Utility function, compare TTL of two database items and return a bool value
 * based on the order of them, true if the first item has a lower delta (e.g.
 * difference between current time and the time of creation) the the second
 * one. Meant to be passed in to a sort function below
 */
static bool compare_ttl(void *arg1, void *arg2) {

    time_t now = time(NULL);

    /* cast to db_item */
    const struct db_item *n1 = ((struct expiring_key *) arg1)->item;
    const struct db_item *n2 = ((struct expiring_key *) arg2)->item;

    time_t delta_l1 = (n1->ctime + n1->ttl) - now;
    time_t delta_l2 = (n2->ctime + n2->ttl) - now;

    return delta_l1 <= delta_l2;
}


static int ttl_handler(struct io_event *event) {

    union triedb_request *packet = event->payload;
    struct client *c = event->client;
    void *val = NULL;

    // Check for key presence in the trie structure
    bool found = trie_find(c->db->data, (const char *) packet->ttl.key, &val);

    if (found == false || val == NULL) {
        event->reply = ack_replies[NOK];
    } else {
        struct db_item *item = val;
        bool has_ttl = !(item->ttl < 0);
        item->ttl = packet->ttl.ttl;

        /*
         * It's a new TTL, so we update creation_time to now in order to
         * calculate the effective expiration of the key
         */
        item->ctime = item->lstime = time(NULL);
        struct expiring_key *ek = tmalloc(sizeof(*ek));
        ek->item = item;
        ek->key = tstrdup((const char *) packet->ttl.key);
        ek->data_ptr = c->db->data;

        /*
         * Push into the expiring keys list and merge sort it shortly after,
         * this way we have a mostly updated list of expiring keys at each
         * insert, making it simpler and more efficient to cycle through them
         * and remove it later.
         */
        if (!has_ttl)
            vector_append(triedb.expiring_keys, ek);

        vector_qsort(triedb.expiring_keys,
                     compare_ttl, sizeof(struct expiring_key));

        event->reply = ack_replies[OK];
    }

    return 0;
}

/*
 * Increment an integer value by 1. If the string value doesn't contain a
 * proper integer return a NOK.
 *
 * XXX check for bounds
 */
static int inc_handler(struct io_event *event) {

    union triedb_request *packet = event->payload;
    struct client *c = event->client;

    if (packet->incr.header.bits.prefix == 1) {
        database_prefix_inc(c->db, (const char *) packet->incr.key);
    } else {

        bool found = false;
        void *val = NULL;

        /* check for presence and increment it by one */
        found = database_search(c->db, (const char *) packet->incr.key, &val);

        if (found == false || !val) {
            event->reply = ack_replies[NOK];
        } else {

            struct db_item *item = val;

            if (!is_integer(item->data))
                event->reply = ack_replies[NOK];
            else
                item->data = update_integer_string(item->data, 1);
        }
    }

    event->reply = ack_replies[OK];

    return 0;
}

/*
 * Decrement an integer value by 1. If the string value doesn't contain a
 * proper integer return a NOK.
 *
 * XXX check for bounds
 */
static int dec_handler(struct io_event *event) {

    union triedb_request *packet = event->payload;
    struct client *c = event->client;

    if (packet->incr.header.bits.prefix == 1) {
        database_prefix_dec(c->db, (const char *) packet->incr.key);
    } else {

        bool found = false;
        void *val = NULL;

        /* check for presence and increment it by one */
        found = database_search(c->db, (const char *) packet->incr.key, &val);

        if (found == false || !val) {
            event->reply = ack_replies[NOK];
        } else {

            struct db_item *item = val;

            if (!is_integer(item->data))
                event->reply = ack_replies[NOK];
            else
                item->data = update_integer_string(item->data, -1);
        }
    }

    event->reply = ack_replies[OK];

    return 0;
}


static int cnt_handler(struct io_event *event) {

    unsigned long long count = 0;
    union triedb_request *packet = event->payload;
    struct client *c = event->client;

    /*
     * Prefix operation by default, get the size of each key below the
     * requested one, glob operation or the entire trie size in case of NULL
     * key
     */
    count = !packet->count.key ? database_size(c->db) :
        database_prefix_count(c->db, (const char *) packet->count.key);

    event->reply = pack_cnt(CNT, count);

    return 0;
}

/* Set the current selected namespace for the connected client. */
static int use_handler(struct io_event *event) {

    union triedb_request *packet = event->payload;
    struct client *c = event->client;

    /* Check for presence first */
    struct database *database =
        hashtable_get(triedb.dbs, (const char *) packet->usec.key);

    /*
     * It doesn't exist, we create a new database with the given name,
     * otherwise just assign it to the current db of the client
     */
    if (!database) {
        // TODO check for OOM
        database = tmalloc(sizeof(*database));
        database_init(database, tstrdup((const char *) packet->usec.key),
                      trie_node_destructor);

        // Add it to the databases table
        hashtable_put(triedb.dbs, tstrdup(database->name), database);
        c->db = database;
    } else {
        c->db = database;
    }

    event->reply = ack_replies[OK];

    return 0;
}


static int keys_handler(struct io_event *event) {

    union triedb_request *packet = event->payload;
    struct client *c = event->client;
    struct get_response *response = NULL;

#if WORKERPOOLSIZE > 1
        pthread_spin_lock(&spinlock);
#endif

        Vector *v = database_prefix_search(c->db,
                                           (const char *) packet->get.key);

#if WORKERPOOLSIZE > 1
        pthread_spin_unlock(&spinlock);
#endif

        /*
         * Prefix request can return either a populated vector with at least
         * one match, or a NULL pointer, in this case we'd change our response
         * to a simple NOK
         */
        if (v)
            response = get_response(packet->get.header.byte, v);
        else {
            event->reply = ack_replies[NOK];
            return 0;
        }

        // XXX Lot of boilerplate, to be refactored

        union triedb_response r = { .get_res = *response };

        event->reply = pack_response(&r, packet->get.header.bits.opcode);

        // TODO destroy response
        if (v) {
            struct kv_obj *current = NULL;
            for (int i = 0; i < vector_size(v); ++i) {
                current = vector_get(v, i);
                tfree((void *) current->key);
                tfree(current);
            }
            tfree(v->items);
            tfree(v);
        }

        tfree(response->tuples);
        tfree(response);

        return 0;
}


static int ping_handler(struct io_event *event) {

    // TODO send out a PONG
    event->reply = ack_replies[OK];

    return 0;
}


static int quit_handler(struct io_event *event) {

    close(event->client->fd);
    info.nclients--;

#if WORKERPOOLSIZE > 1
    pthread_spin_lock(&spinlock);
#endif

    // Remove client from the clients map
    hashtable_del(triedb.clients, event->client->uuid);

#if WORKERPOOLSIZE > 1
    pthread_spin_unlock(&spinlock);
#endif

    return -1;
}


static int info_handler(struct io_event *event) {

    // XXX placeholder
    // TODO
    event->reply = pack_info(conf, &info);

    return 0;
}


static int flush_handler(struct io_event *event) {

#if WORKERPOOLSIZE > 1
    pthread_spin_lock(&spinlock);
#endif

    // Flush the entire DB
    database_flush(event->client->db);

#if WORKERPOOLSIZE > 1
    pthread_spin_unlock(&spinlock);
#endif

    return 0;
}

/* Utility macro to handle base case on each EPOLL loop */
#define EPOLL_ERR(e) if ((e.events & EPOLLERR) || (e.events & EPOLLHUP) || \
                         (!(e.events & EPOLLIN) && !(e.events & EPOLLOUT)))

/*
 * Handle incoming connections, create a a fresh new struct client structure
 * and link it to the fd, ready to be set in EPOLLIN event, then pass the
 * connection to the IO EPOLL loop, waited by the IO thread pool.
 */
static void accept_loop(struct epoll *epoll) {

    int events = 0;

    struct epoll_event *e_events =
        tmalloc(sizeof(struct epoll_event) * EPOLL_MAX_EVENTS);

    int epollfd = epoll_create1(0);

    /*
     * We want to watch for events incoming on the server descriptor (e.g. new
     * connections)
     */
    epoll_add(epollfd, epoll->serverfd, EPOLLIN | EPOLLONESHOT, NULL);

    /*
     * And also to the global event fd, this one is useful to gracefully
     * interrupt polling and thread execution
     */
    epoll_add(epollfd, conf->run, EPOLLIN, NULL);

    while (1) {

        events = epoll_wait(epollfd, e_events, EPOLL_MAX_EVENTS, EPOLL_TIMEOUT);

        if (events < 0) {

            /* Signals to all threads. Ignore it for now */
            if (errno == EINTR)
                continue;

            /* Error occured, break the loop */
            break;
        }

        for (int i = 0; i < events; ++i) {

            /* Check for errors */
            EPOLL_ERR(e_events[i]) {

                /*
                 * An error has occured on this fd, or the socket is not
                 * ready for reading, closing connection
                 */
                perror ("epoll_wait(2)");
                close(e_events[i].data.fd);

            } else if (e_events[i].data.fd == conf->run) {

                /* And quit event after that */
                eventfd_t val;
                eventfd_read(conf->run, &val);

                tdebug("Stopping epoll loop. Thread %p exiting.",
                       (void *) pthread_self());

                goto exit;

            } else if (e_events[i].data.fd == epoll->serverfd) {

                while (1) {

                    /*
                     * Accept a new incoming connection assigning ip address
                     * and socket descriptor to the connection structure
                     * pointer passed as argument
                     */

                    int fd = accept_connection(epoll->serverfd);
                    if (fd < 0)
                        break;

                    /*
                     * Create a client structure to handle his context
                     * connection
                     */
                    struct client *client = tmalloc(sizeof(struct client));
                    if (!client)
                        oom("creating client during accept");

                    /* Generate random uuid */
                    uuid_t binuuid;
                    uuid_generate_random(binuuid);
                    uuid_unparse(binuuid, (char *) client->uuid);

                    /* Populate client structure */
                    client->fd = fd;

                    /* Record last action as of now */
                    client->last_action_time = (uint64_t) time(NULL);

                    /* Set the default db for the current user */
                    client->db = hashtable_get(triedb.dbs, "db0");

                    /* Add it to the db instance */
                    hashtable_put(triedb.clients, client->uuid, client);

                    /* Add it to the epoll loop */
                    epoll_add(epoll->io_epollfd, fd,
                              EPOLLIN | EPOLLONESHOT, client);

                    /* Rearm server fd to accept new connections */
                    epoll_mod(epollfd, epoll->serverfd, EPOLLIN, NULL);

                    /* Record the new client connected */
                    info.nclients++;
                    info.nconnections++;

                }
            }
        }
    }

exit:

    tfree(e_events);
}

/* Handle incoming requests, after being accepted or after a reply */
static int read_data(int fd, unsigned char *buffer, union triedb_request *pkt) {

    ssize_t bytes = 0;
    unsigned char header = 0;

    /*
     * We must read all incoming bytes till an entire packet is received. This
     * is achieved by following the TrieDB protocol specifications, which
     * send the size of the remaining packet as the second byte. By knowing it
     * we know if the packet is ready to be deserialized and used.
     */
    bytes = recv_packet(fd, &buffer, &header);

    /*
     * Looks like we got a client disconnection.
     *
     * TODO: Set a error_handler for ERRMAXREQSIZE instead of dropping client
     *       connection, explicitly returning an informative error code to the
     *       client connected.
     */
    if (bytes == -ERRCLIENTDC || bytes == -ERRMAXREQSIZE)
        goto errdc;

    /*
     * If a not correct packet received, we must free the buffer and reset the
     * handler to the request again, setting EPOLL to EPOLLIN
     */
    if (bytes == -ERRPACKETERR)
        goto exit;

    info.bytes_recv += bytes;

    /*
     * Unpack received bytes into a triedb_request structure and execute the
     * correct handler based on the type of the operation.
     */
    unpack_triedb_request(buffer, pkt, header, bytes);

    return 0;

exit:

    return -ERRPACKETERR;

errdc:

    terror("Dropping client");
    close(fd);

    info.nclients--;

    info.nconnections--;

    return -ERRCLIENTDC;
}

/* Handle incoming packets from other nodes */
static int read_packet(unsigned char *buffer, union triedb_packet *pkt) {

    unsigned char header = 0;
    const unsigned char *p = buffer;
    header = *p;
    p++;
    unsigned pos = 0;
    size_t bytes = decode_length(&p, &pos);

    // Check for second LSB, where the type of packet is stored
    if (((header >> 7) & 1) == 1) {

        // we got a request
        union triedb_request req;

        /*
         * Unpack received bytes into a triedb_request structure and
         * execute the correct handler based on the type of the
         * operation.
         */
        unpack_triedb_request(p, &req, header, bytes);

        pkt->request = req;
    } else {
        // we got a response
        union triedb_response res;

        unpack_triedb_response(p, &res, header, bytes);
        pkt->response = res;
    }

    return 0;
}


#define BUFSIZE 2048


static void *io_worker(void *arg) {

    struct epoll *epoll = arg;
    int events = 0;
    ssize_t sent = 0;

    struct epoll_event *e_events =
        tmalloc(sizeof(struct epoll_event) * EPOLL_MAX_EVENTS);

    /* Raw bytes buffer to handle input from client */
    unsigned char *buffer = tmalloc(conf->max_request_size);

    // UDP bus communication client handler
    struct sockaddr_in node;
    memset(&node, 0, sizeof(node)); ;

    while (1) {

        events = epoll_wait(epoll->io_epollfd, e_events,
                            EPOLL_MAX_EVENTS, EPOLL_TIMEOUT);

        if (events < 0) {

            /* Signals to all threads. Ignore it for now */
            if (errno == EINTR)
                continue;

            /* Error occured, break the loop */
            break;
        }

        for (int i = 0; i < events; ++i) {

            /* Check for errors */
            EPOLL_ERR(e_events[i]) {

                /* An error has occured on this fd, or the socket is not
                   ready for reading, closing connection */
                perror ("epoll_wait(2)");
                close(e_events[i].data.fd);

            } else if (e_events[i].data.fd == conf->run) {

                /* And quit event after that */
                eventfd_t val;
                eventfd_read(conf->run, &val);

                tdebug("Stopping epoll loop. Thread %p exiting.",
                       (void *) pthread_self());

                goto exit;

            } else if (epoll->busfd > 0 && e_events[i].data.fd == epoll->busfd) {
                // Bus communication from UDP chanel
                int n;
                socklen_t len;
                n = recvfrom(e_events[i].data.fd, (char *)buffer, BUFSIZE,
                             MSG_WAITALL, ( struct sockaddr *) &node, &len);
                buffer[n] = '\0';

                unsigned char header = 0;

                union triedb_request *pkt = tmalloc(sizeof(*pkt));

                const unsigned char *p = buffer;
                header = *p;
                p++;
                unsigned pos = 0;
                size_t bytes = decode_length(&p, &pos);

                /*
                 * Unpack received bytes into a triedb_request structure and
                 * execute the correct handler based on the type of the
                 * operation.
                 */
                unpack_triedb_request(p, pkt, header, bytes);

                info.nnodes++;

                // TODO: Split reception and unpacking of node based on the
                // packet type, to handle responses and requests as well
                tdebug("Received JOIN");

            } else if (e_events[i].events & EPOLLIN) {
                struct io_event *event = tmalloc(sizeof(*event));
                event->epollfd = epoll->io_epollfd;
                event->payload = tmalloc(sizeof(*event->payload));
                event->client = e_events[i].data.ptr;
                /*
                 * Received a bunch of data from a client, after the creation
                 * of an IO event we need to read the bytes and encoding the
                 * content according to the protocol
                 */
                int rc = read_data(event->client->fd, buffer, event->payload);
                if (rc == 0) {
                    /*
                     * All is ok, raise an event to the worker poll EPOLL and
                     * link it with the IO event containing the decode payload
                     * ready to be processed
                     */
                    eventfd_t ev = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
                    event->io_event = ev;
                    epoll_add(epoll->w_epollfd, ev,
                              EPOLLIN | EPOLLONESHOT, event);
                    eventfd_write(ev, 1);
                }
                else if (rc == -ERRCLIENTDC) {

                    /*
                     * We got an unexpected error or a disconnection from the
                     * client side, remove client from the global map and
                     * free resources allocated such as io_event structure and
                     * paired payload
                     */

#if WORKERPOOLSIZE > 1
                    pthread_spin_lock(&spinlock);
#endif

                    close(event->client->fd);
                    hashtable_del(triedb.clients, event->client->uuid);
                    tfree(event->payload);
                    tfree(event);

#if WORKERPOOLSIZE > 1
                    pthread_spin_unlock(&spinlock);
#endif
                }

                /* Record last action as of now */
                event->client->last_action_time = (uint64_t) time(NULL);

            } else if (e_events[i].events & EPOLLOUT) {

                struct io_event *event = e_events[i].data.ptr;

                /*
                 * Write out to client, after a request has been processed in
                 * worker thread routine. Just send out all bytes stored in the
                 * reply buffer to the reply file descriptor.
                 */
                if ((sent = send_bytes(event->client->fd,
                                       (const unsigned char *) event->reply,
                                       bstring_len(event->reply))) < 0) {
                    close(event->client->fd);
                }

                // Update information stats
                info.bytes_sent += sent < 0 ? 0 : sent;

                /*
                 * Rearm descriptor, we're using EPOLLONESHOT feature to avoid
                 * race condition and thundering herd issues on multithreaded
                 * EPOLL
                 */
                epoll_mod(epoll->io_epollfd,
                          event->client->fd, EPOLLIN, event->client);

                /* Free resource, ACKs will be free'd closing the server */
                if ((*event->reply >> 4) != ACK)
                    bstring_destroy(event->reply);

                tfree(event);
            }
        }
    }

exit:

    tfree(e_events);
    tfree(buffer);

    return NULL;
}


static void *worker(void *arg) {

    struct epoll *epoll = arg;
    int events = 0;
    long int timers = 0;
    eventfd_t val;

    struct epoll_event *e_events =
        tmalloc(sizeof(struct epoll_event) * EPOLL_MAX_EVENTS);

    while (1) {

        events = epoll_wait(epoll->w_epollfd, e_events,
                            EPOLL_MAX_EVENTS, EPOLL_TIMEOUT);

        if (events < 0) {

            /* Signals to all threads. Ignore it for now */
            if (errno == EINTR)
                continue;

            /* Error occured, break the loop */
            break;
        }

        for (int i = 0; i < events; ++i) {

            /* Check for errors */
            EPOLL_ERR(e_events[i]) {

                /*
                 * An error has occured on this fd, or the socket is not
                 * ready for reading, closing connection
                 */
                perror ("epoll_wait(2)");
                close(e_events[i].data.fd);

            } else if (e_events[i].data.fd == conf->run) {

                /* And quit event after that */
                eventfd_read(conf->run, &val);

                tdebug("Stopping epoll loop. Thread %p exiting.",
                       (void *) pthread_self());

                goto exit;

            } else if (e_events[i].data.fd == epoll->expirefd) {
                (void) read(e_events[i].data.fd, &timers, sizeof(timers));
                // Check for keys about to expire out
                expire_keys();
            } else if (e_events[i].events & EPOLLIN) {
                struct io_event *event = e_events[i].data.ptr;
                eventfd_read(event->io_event, &val);
                // TODO free client and remove it from the global map in case
                // of QUIT command (check return code)
                handlers[event->payload->header.bits.opcode](event);
                epoll_mod(event->epollfd, event->client->fd, EPOLLOUT, event);
                close(event->io_event);
                triedb_request_destroy(event->payload);
            }
        }
    }

exit:

    tfree(e_events);

    return NULL;
}

/*
 * Parse packet header, it is required at least the Fixed Header of each
 * packed, which is contained in the first 2 bytes in order to read packet
 * type and total length that we need to recv to complete the packet.
 *
 * This function accept a socket fd, a buffer to read incoming streams of
 * bytes and a structure formed by 2 fields:
 *
 * - buf -> a byte buffer, it will be malloc'ed in the function and it will
 *          contain the serialized bytes of the incoming packet
 * - flags -> flags pointer, copy the flag setting of the incoming packet,
 *            again for simplicity and convenience of the caller.
 */
ssize_t recv_packet(int clientfd, unsigned char **buf, unsigned char *header) {

    ssize_t nbytes = 0;
    unsigned char *tmpbuf = *buf;

    /* Read the first byte, it should contain the message type code */
    if ((nbytes = recv_bytes(clientfd, *buf, 4)) <= 0)
        return -ERRCLIENTDC;

    *header = *tmpbuf;
    tmpbuf++;

    /* Check for OPCODE, if an unknown OPCODE is received return an error */
    if (INFO < (*header >> 4) || PUT > (*header >> 4))
        return -ERRPACKETERR;

    /*
     * Read remaning length bytes which starts at byte 2 and can be long to 4
     * bytes based on the size stored, so byte 2-5 is dedicated to the packet
     * length.
     */
    int n = 0;

    unsigned pos = 0;
    unsigned long long tlen = decode_length((const unsigned char **) &tmpbuf,
                                            &pos);
    /*
     * Set return code to -ERRMAXREQSIZE in case the total packet len exceeds
     * the configuration limit `max_request_size`
     */
    if (tlen > conf->max_request_size) {
        nbytes = -ERRMAXREQSIZE;
        goto exit;
    }

    if (tlen <= 4)
        goto exit;

    /* Read remaining bytes to complete the packet */
    if ((n = recv_bytes(clientfd, tmpbuf + pos + 1, tlen - pos)) < 0)
        goto err;

    nbytes += n - pos - 1;

exit:

    *buf += pos + 1;

    return nbytes;

err:

    close(clientfd);

    return nbytes;

}

/*
 * Cycle through sorted list of expiring keys and remove those which are
 * elegible. Meant to be run as a cron routine, several times per second.
 */
static void expire_keys(void) {

    if (vector_size(triedb.expiring_keys) == 0)
        return;

    time_t now = time(NULL);
    time_t delta = 0LL;
    struct expiring_key *ek = NULL;

#if WORKERPOOLSIZE > 1
    pthread_spin_lock(&spinlock);
#endif

    for (int i = 0; i < vector_size(triedb.expiring_keys); ++i) {

        ek = vector_get(triedb.expiring_keys, i);
        delta = (ek->item->ctime + ek->item->ttl) - now;

        /*
         * Vector *should be* sorted per time delta value, so it is futile to
         * go on after the first positive timedelta encoutered
         */
        if (delta > 0)
            break;

        /*
         * ek->data_ptr points to the trie of the client which stores the given
         * key
         */
        trie_delete(ek->data_ptr, ek->key);

        vector_delete(triedb.expiring_keys, i);

        // Update total keyspace counter
        triedb.keyspace_size--;

        tdebug("%s expired", ek->key);

        tfree((char *) ek->key);
        tfree(ek);
        ek = NULL;
    }

#if WORKERPOOLSIZE > 1
    pthread_spin_unlock(&spinlock);
#endif

}

/* Hashtable destructor function for struct client objects. */
static inline int client_destructor(struct hashtable_entry *entry) {

    if (!entry || !entry->val)
        return -HASHTABLE_ERR;

    struct client *client = entry->val;

    tfree(client);

    return HASHTABLE_OK;
}

/*
 * Hashtable destructor function for struct database objects. It's the function
 * that will be called on hashtable_del call as well as hashtable_destroy too.
 */
static inline int database_destructor(struct hashtable_entry *entry) {

    if (!entry)
        return -HASHTABLE_ERR;

    struct database *db = entry->val;

    tfree((char *) (db->name));

    trie_destroy(db->data);

    tfree(entry->val);
    tfree((char *) entry->key);

    return HASHTABLE_OK;
}

/*
 * Trie destructor function for struct trie_node obejcts. It's the function
 * that will be called on trie_del call as well as trie_destroy too.
 * In our case each node will contain a db_item structure so we free resources
 * according to this structure.
 * Accepts a `dataonly` boolean, beside the poor name, it is used by trie
 * datastructure to delete nodes singularly or "in batch", a call of
 * trie_delete will this way preserve integrity on the structure without
 * de-allocating memory on non-leaf nodes.
 */
static inline bool trie_node_destructor(struct trie_node *node,
                                        bool dataonly) {
    bool ret = false;

    if (!node)
        return ret;

    struct db_item *item = node->data;

    if (!item)
        goto exit;

    if (item->data) {
        tfree(item->data);
        item->data = NULL;
    }

    tfree(node->data);
    node->data = NULL;

    ret = true;

exit:

    if (dataonly == false)
        tfree(node);

    return ret;
}

/*
 * Vector destructor function for struct expiring_key objects. It's the
 * function that will be called on Vector remove or vector_destroy
 */
static inline void expiring_keys_destructor(void *item) {

    if (!item)
        return;

    struct expiring_key *ek = item;

    if (ek->key)
        tfree((char *) ek->key);

    tfree(ek);

}

/* Init informations structure, to be run before starting the server */
static void init_info(void) {
    info.nclients = 0;
    info.bytes_recv = 0;
    info.bytes_sent = 0;
    info.nconnections = 0;
    info.nkeys = 0;
    info.nrequests = 0;
    info.uptime = 0;
}

/*
 * Main entry point for start listening on a socket and running an epoll event
 * loop to accept new connections.
 * Accepts two mandatory string arguments, addr and port, in case of UNIX
 * domain socket, addr represents the path on the FS where the socket fd is
 * located, port will be ignored.
 */
int start_server(const char *addr, const char *port, struct seednode *seed) {

    init_info();

    /* Populate the static ACK replies */
    for (int i = 0; i < 3; ++i)
        ack_replies[i] = pack_ack(ACK, i);

#if WORKERPOOLSIZE > 1
    pthread_spin_init(&spinlock, PTHREAD_PROCESS_SHARED);
#endif

    /* Create default database */
    struct database *default_db = tmalloc(sizeof(struct database));
    database_init(default_db, tstrdup("db0"), trie_node_destructor);

    /* Initialize global triedb instance */
    triedb.dbs = hashtable_new(database_destructor);
    triedb.clients = hashtable_new(client_destructor);
    triedb.expiring_keys = vector_new(expiring_keys_destructor);
    triedb.cluster = &(struct cluster) { 0, 4, list_new(NULL) };

    /* Add it to the global map */
    hashtable_put(triedb.dbs, tstrdup(default_db->name), default_db);

    /* Start listening for new connections */
    int sfd = make_listen(addr, port, conf->socket_family);

    struct epoll epoll = {
        .io_epollfd = epoll_create1(0),
        .w_epollfd = epoll_create1(0),
        .serverfd = sfd,
        .busfd = -1
    };

    /* Start the expiration keys check routine */
    struct itimerspec timervalue;

    memset(&timervalue, 0x00, sizeof(timervalue));

    timervalue.it_value.tv_sec = 0;
    timervalue.it_value.tv_nsec = TTL_CHECK_INTERVAL;
    timervalue.it_interval.tv_sec = 0;
    timervalue.it_interval.tv_nsec = TTL_CHECK_INTERVAL;

    // add expiration keys cron task
    int exptimerfd = add_cron_task(epoll.w_epollfd, &timervalue);

    epoll.expirefd = exptimerfd;

    /*
     * We need to watch for global eventfd in order to gracefully shutdown IO
     * thread pool and worker pool
     */
    epoll_add(epoll.io_epollfd, conf->run, EPOLLIN, NULL);
    epoll_add(epoll.w_epollfd, conf->run, EPOLLIN, NULL);

    pthread_t iothreads[IOPOOLSIZE];
    pthread_t workers[WORKERPOOLSIZE];

    /* Start I/O thread pool */

    for (int i = 0; i < IOPOOLSIZE; ++i)
        pthread_create(&iothreads[i], NULL, &io_worker, &epoll);

    /* Start Worker thread pool */

    for (int i = 0; i < WORKERPOOLSIZE; ++i)
        pthread_create(&workers[i], NULL, &worker, &epoll);

    /*
     * If it is run in CLUSTER mode add an additional descriptor and register
     * it to the event loop, ready to accept incoming connections from other
     * tritedb nodes and handle cluster commands.
     */
    if (conf->mode == CLUSTER) {

        /* Add 10k to the listening server port */
        int busport = atoi(port) + 10000;
        char bport[6];
        snprintf(bport, number_len(busport), "%d", busport);

        /* The bus one for distribution */
        int busfd = create_and_bind(addr, bport, UDP);

        set_nonblocking(busfd);

        epoll.busfd = busfd;

        /* Set bus socket in EPOLLIN too */
        epoll_add(epoll.io_epollfd, busfd, EPOLLIN, NULL);

        cluster_add_new_node(triedb.cluster, busfd, addr, bport, true);

        info.nnodes++;

        tdebug("Joined a cluster");
    }

    /*
     * Add socket for bus communication if accepted by a seed node
     * TODO make it in another thread or better, crate a usable client
     * structure like if it was accepted as a new connection, cause actually
     * it crashes the server by having NULL ptr
     */
    if (seed->connect) {

        int sockfd;
        struct sockaddr_in servaddr;

        // Creating socket file descriptor
        if ((sockfd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
            perror("socket creation failed");
            exit(EXIT_FAILURE);
        }

        memset(&servaddr, 0, sizeof(servaddr));
        int tport = atoi(seed->port) + 10000;

        // Filling server information
        servaddr.sin_family = AF_INET;
        servaddr.sin_port = htons(tport);
        servaddr.sin_addr.s_addr = INADDR_ANY;

        union header header = {.byte = 0};
        header.bits.opcode = JOIN;
        bstring payload = pack_ack(header.byte, 0);

        sendto(sockfd, (const char *) payload, bstring_len(payload),
               MSG_CONFIRM, (const struct sockaddr *) &servaddr,
               sizeof(servaddr));

        // Add target node as cluster node
        cluster_add_new_node(triedb.cluster, sockfd,
                             seed->addr, seed->port, false);

        tdebug("New node on %s:%s joined", seed->addr, seed->port);

        bstring_destroy(payload);
    }

    tinfo("Server start");
    info.start_time = time(NULL);

    // Main thread for accept new connections
    accept_loop(&epoll);

    // Stop expire keys check routine
    epoll_del(epoll.w_epollfd, epoll.expirefd);

    /* Join started thread pools */
    for (int i = 0; i < IOPOOLSIZE; ++i)
        pthread_join(iothreads[i], NULL);

    for (int i = 0; i < WORKERPOOLSIZE; ++i)
        pthread_join(workers[i], NULL);

    /* Free all allocated resources */
    hashtable_destroy(triedb.dbs);
    hashtable_destroy(triedb.clients);
    vector_destroy(triedb.expiring_keys);
    list_destroy(triedb.cluster->nodes, 1);

    for (int i = 0; i < 3; ++i)
        bstring_destroy(ack_replies[i]);

    tinfo("triedb v%s exiting", VERSION);

    return 0;
}
