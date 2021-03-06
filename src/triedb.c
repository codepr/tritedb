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

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/eventfd.h>
#include "server.h"
#include "network.h"
#include "util.h"
#include "config.h"


// Stops epoll_wait loops by sending an event
void sigint_handler(int signum) {
    printf("\n");
    for (int i = 0; i < IOPOOLSIZE + WORKERPOOLSIZE + 1; ++i) {
        eventfd_write(conf->run, 1);
        usleep(1500);
    }
}


int main(int argc, char **argv) {

    signal(SIGINT, sigint_handler);
    signal(SIGTERM, sigint_handler);

    /* Intialize random seed */
    srand((unsigned int) time(NULL));

    char *addr = DEFAULT_HOSTNAME;
    char *port = DEFAULT_PORT;
    char *confpath = DEFAULT_CONF_PATH;
    char *mode = "STANDALONE";
    int debug = 0;
    int opt;

    // Set default configuration
    config_set_default();

    while ((opt = getopt(argc, argv, "a:c:p:m:vn:")) != -1) {
        switch (opt) {
            case 'a':
                addr = optarg;
                strcpy(conf->hostname, addr);
                break;
            case 'c':
                confpath = optarg;
                break;
            case 'p':
                port = optarg;
                strcpy(conf->port, port);
                break;
            case 'm':
                mode = optarg;
                conf->mode = STREQ(mode, "CLUSTER", 7) ? CLUSTER : STANDALONE;
                break;
            case 'v':
                debug = 1;
                break;
            default:
                fprintf(stderr, "Usage: %s [-a addr] [-p port] [-m mode] [-c conf] [-v]\n", argv[0]);
                exit(EXIT_FAILURE);
        }
    }

    // Override default DEBUG mode
    conf->loglevel = debug == 1 ? DEBUG : WARNING;

    // Try to load a configuration, if found
    config_load(confpath);

    // 22 magic value, the max length + 1 for nul
    char fulladdr[22];

    strcpy(fulladdr, addr);

    struct seednode seed;
    seed.connect = false;
    strcpy((char *) seed.addr, addr);
    if (optind < argc) {
        if (STREQ(argv[optind], "join", 4) == 0) {
            // Target is the pair target_host:port+10000
            char *target = argv[optind + 1];
            int tport = atoi(argv[optind + 2]) + 10000;

            strcpy((char *) seed.port, argv[optind + 2]);

            strcpy((char *) seed.fulladdr,
                   strcat(fulladdr, argv[optind + 2]));

            seed.connect = true;
        }
    }

    // Initialize logging
    t_log_init(conf->logpath);

    config_print();

    // Start
    start_server(conf->hostname, conf->port, &seed);

    // Close logger
    t_log_close();

    return 0;
}
