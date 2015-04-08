/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2015 Canonical

  Author:
    Didier Roche <didrocks@ubuntu.com>

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU Lesser General Public License as published by
  the Free Software Foundation; either version 2.1 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <getopt.h>
#include <errno.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include "build.h"
#include "event-util.h"
#include "fsckd.h"
#include "log.h"
#include "list.h"
#include "macro.h"
#include "sd-daemon.h"
#include "socket-util.h"
#include "util.h"

#define IDLE_TIME_SECONDS 30

struct Manager;

typedef struct Client {
        struct Manager *manager;
        int fd;
        dev_t devnum;
        size_t cur;
        size_t max;
        int pass;
        double percent;
        size_t buflen;

        LIST_FIELDS(struct Client, clients);
} Client;

typedef struct Manager {
        sd_event *event;
        Client *clients;
        int clear;
        int connection_fd;
        FILE *console;
        double percent;
        int numdevices;
} Manager;

static void manager_free(Manager *m);
DEFINE_TRIVIAL_CLEANUP_FUNC(Manager*, manager_free);
#define _cleanup_manager_free_ _cleanup_(manager_freep)

static double compute_percent(int pass, size_t cur, size_t max) {
        /* Values stolen from e2fsck */

        static const double pass_table[] = {
                0, 70, 90, 92, 95, 100
        };

        if (pass <= 0)
                return 0.0;

        if ((unsigned) pass >= ELEMENTSOF(pass_table) || max == 0)
                return 100.0;

        return pass_table[pass-1] +
                (pass_table[pass] - pass_table[pass-1]) *
                (double) cur / max;
}


static void remove_client(Client **first, Client *item) {
        LIST_REMOVE(clients, *first, item);
        safe_close(item->fd);
        free(item);
}

static int update_global_progress(Manager *m) {
        Client *current = NULL;
        _cleanup_free_ char *console_message = NULL;
        int current_numdevices = 0, l = 0;
        double current_percent = 100;

        /* get the overall percentage */
        LIST_FOREACH(clients, current, m->clients) {
                current_numdevices++;

                /* right now, we only keep the minimum % of all fsckd processes. We could in the future trying to be
                   linear, but max changes and corresponds to the pass. We have all the informations into fsckd
                   already if we can treat that in a smarter way. */
                current_percent = MIN(current_percent, current->percent);
        }

        /* update if there is anything user-visible to update */
        if (fabs(current_percent - m->percent) > 0.001 || current_numdevices != m->numdevices) {
                m->numdevices = current_numdevices;
                m->percent = current_percent;

                if (asprintf(&console_message, "Checking in progress on %d disks (%3.1f%% complete)",
                                                m->numdevices, m->percent) < 0)
                        return -ENOMEM;

                /* write to console */
                if (m->console) {
                        fprintf(m->console, "\r%s\r%n", console_message, &l);
                        fflush(m->console);
                }

                if (l > m->clear)
                        m->clear = l;
        }
        return 0;
}

static int progress_handler(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
        Client *client = userdata;
        Manager *m = NULL;
        FsckProgress fsck_data;
        size_t buflen;
        int r;

        assert(client);
        m = client->manager;

        /* ensure we have enough data to read */
        r = ioctl(fd, FIONREAD, &buflen);
        if (r == 0 && buflen != 0 && (size_t) buflen < sizeof(FsckProgress)) {
                if (client->buflen != buflen)
                        client->buflen = buflen;
                /* we got twice the same size from a bad behaving client, kick it off the list */
                else {
                        log_warning("Closing bad behaving fsck client connection at fd %d", client->fd);
                        remove_client(&(m->clients), client);
                        r = update_global_progress(m);
                        if (r < 0)
                                log_warning_errno(r, "Couldn't update global progress: %m");
                }
                return 0;
        }

        /* read actual data */
        r = recv(fd, &fsck_data, sizeof(FsckProgress), 0);
        if (r == 0) {
                log_debug("Fsck client connected to fd %d disconnected", client->fd);
                remove_client(&(m->clients), client);
        } else if (r > 0 && r != sizeof(FsckProgress))
                log_warning("Unexpected data structure sent to fsckd socket from fd: %d. Ignoring", client->fd);
        else if (r > 0 && r == sizeof(FsckProgress)) {
                client->devnum = fsck_data.devnum;
                client->cur = fsck_data.cur;
                client->max = fsck_data.max;
                client->pass = fsck_data.pass;
                client->percent = compute_percent(client->pass, client->cur, client->max);
                log_debug("Getting progress for %u:%u (%lu, %lu, %d) : %3.1f%%",
                          major(client->devnum), minor(client->devnum),
                          client->cur, client->max, client->pass, client->percent);
        } else
                log_error_errno(r, "Unknown error while trying to read fsck data: %m");

        r = update_global_progress(m);
        if (r < 0)
                log_warning_errno(r, "Couldn't update global progress: %m");

        return 0;
}

static int new_connection_handler(sd_event_source *s, int fd, uint32_t revents, void *userdata) {
        Manager *m = userdata;
        Client *client = NULL;
        int new_client_fd, r;

        assert(m);

        /* Initialize and list new clients */
        new_client_fd = accept4(m->connection_fd, NULL, NULL, SOCK_CLOEXEC);
        if (new_client_fd > 0) {
                log_debug("New fsck client connected to fd: %d", new_client_fd);
                client = new0(Client, 1);
                if (!client)
                        return log_oom();
                client->fd = new_client_fd;
                client->manager = m;
                LIST_PREPEND(clients, m->clients, client);
                r = sd_event_add_io(m->event, NULL, client->fd, EPOLLIN, progress_handler, client);
                if (r < 0) {
                        remove_client(&(m->clients), client);
                        return r;
                }
        } else
                return log_error_errno(errno, "Couldn't accept a new connection: %m");

        return 0;
}

static void manager_free(Manager *m) {
        Client *current = NULL, *l = NULL;
        if (!m)
                return;

        /* clear last line */
        if (m->console && m->clear > 0) {
                unsigned j;

                fputc('\r', m->console);
                for (j = 0; j < (unsigned) m->clear; j++)
                        fputc(' ', m->console);
                fputc('\r', m->console);
                fflush(m->console);
        }

        safe_close(m->connection_fd);
        if (m->console)
                fclose(m->console);

        LIST_FOREACH_SAFE(clients, current, l, m->clients)
                remove_client(&(m->clients), current);

        sd_event_unref(m->event);

        free(m);
}

static int manager_new(Manager **ret, int fd) {
        _cleanup_manager_free_ Manager *m = NULL;
        int r;

        assert(ret);

        m = new0(Manager, 1);
        if (!m)
                return -ENOMEM;

        r = sd_event_default(&m->event);
        if (r < 0)
                return r;

        m->connection_fd = fd;
        m->console = fopen("/dev/console", "we");
        if (!m->console)
                return log_warning_errno(errno, "Can't connect to /dev/console: %m");
        m->percent = 100;

        *ret = m;
        m = NULL;

        return 0;
}

static int run_event_loop_with_timeout(sd_event *e, usec_t timeout) {
        int r, code;

        assert(e);

        for (;;) {
                r = sd_event_get_state(e);
                if (r < 0)
                        return r;
                if (r == SD_EVENT_FINISHED)
                        break;

                r = sd_event_run(e, timeout);
                if (r < 0)
                        return r;

                /* timeout reached */
                if (r == 0) {
                        sd_event_exit(e, 0);
                        break;
                }
        }

        r = sd_event_get_exit_code(e, &code);
        if (r < 0)
                return r;

        return code;
}

static void help(void) {
        printf("%s [OPTIONS...]\n\n"
               "Capture fsck progress and forward one stream to plymouth\n\n"
               "  -h --help             Show this help\n"
               "     --version          Show package version\n",
               program_invocation_short_name);
}

static int parse_argv(int argc, char *argv[]) {

        enum {
                ARG_VERSION = 0x100,
                ARG_ROOT,
        };

        static const struct option options[] = {
                { "help",      no_argument,       NULL, 'h'           },
                { "version",   no_argument,       NULL, ARG_VERSION   },
                {}
        };

        int c;

        assert(argc >= 0);
        assert(argv);

        while ((c = getopt_long(argc, argv, "hv", options, NULL)) >= 0)
                switch (c) {

                case 'h':
                        help();
                        return 0;

                case ARG_VERSION:
                        puts(PACKAGE_STRING);
                        puts(SYSTEMD_FEATURES);
                        return 0;

                case '?':
                        return -EINVAL;

                default:
                        assert_not_reached("Unhandled option");
                }

        if (optind < argc) {
                log_error("Extraneous arguments");
                return -EINVAL;
        }

        return 1;
}

int main(int argc, char *argv[]) {
        _cleanup_manager_free_ Manager *m = NULL;
        int fd = -1;
        int r, n;

        log_set_target(LOG_TARGET_AUTO);
        log_parse_environment();
        log_open();

        r = parse_argv(argc, argv);
        if (r <= 0)
                return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;

        n = sd_listen_fds(0);
        if (n > 1) {
                log_error("Too many file descriptors received.");
                return EXIT_FAILURE;
        } else if (n == 1) {
                fd = SD_LISTEN_FDS_START + 0;
        } else {
                fd = make_socket_fd(LOG_DEBUG, FSCKD_SOCKET_PATH, SOCK_STREAM | SOCK_CLOEXEC);
                if (fd < 0) {
                        log_error_errno(r, "Couldn't create listening socket fd on %s: %m", FSCKD_SOCKET_PATH);
                        return EXIT_FAILURE;
                }
        }

        r = manager_new(&m, fd);
        if (r < 0) {
                log_error_errno(r, "Failed to allocate manager: %m");
                return EXIT_FAILURE;
        }

        r = sd_event_add_io(m->event, NULL, fd, EPOLLIN, new_connection_handler, m);
        if (r < 0) {
                log_error_errno(r, "Can't listen to connection socket: %m");
                return EXIT_FAILURE;
        }

        r = run_event_loop_with_timeout(m->event, IDLE_TIME_SECONDS * USEC_PER_SEC);
        if (r < 0) {
                log_error_errno(r, "Failed to run event loop: %m");
                return EXIT_FAILURE;
        }

        sd_event_get_exit_code(m->event, &r);

        return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
