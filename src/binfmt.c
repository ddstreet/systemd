/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

/***
  This file is part of systemd.

  Copyright 2010 Lennart Poettering

  systemd is free software; you can redistribute it and/or modify it
  under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  systemd is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with systemd; If not, see <http://www.gnu.org/licenses/>.
***/

#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

#include "log.h"
#include "util.h"

static int delete_rule(const char *rule) {
        char *x, *fn, *e;
        int r;

        assert(rule[0]);

        if (!(x = strdup(rule)))
                return -ENOMEM;

        e = strchrnul(x+1, x[0]);
        *e = 0;

        asprintf(&fn, "/proc/sys/fs/binfmt_misc/%s", x+1);
        free(x);

        if (!fn)
                return -ENOMEM;

        r = write_one_line_file(fn, "-1");
        free(fn);

        return r;
}

static int apply_rule(const char *rule) {
        int r;

        delete_rule(rule);

        if ((r = write_one_line_file("/proc/sys/fs/binfmt_misc/register", rule)) < 0) {
                log_error("Failed to add binary format: %s", strerror(-r));
                return r;
        }

        return 0;
}

static int apply_file(const char *path, bool ignore_enoent) {
        FILE *f;
        int r = 0;

        assert(path);

        if (!(f = fopen(path, "re"))) {
                if (ignore_enoent && errno == ENOENT)
                        return 0;

                log_error("Failed to open file '%s', ignoring: %m", path);
                return -errno;
        }

        while (!feof(f)) {
                char l[LINE_MAX], *p;
                int k;

                if (!fgets(l, sizeof(l), f)) {
                        if (feof(f))
                                break;

                        log_error("Failed to read file '%s', ignoring: %m", path);
                        r = -errno;
                        goto finish;
                }

                p = strstrip(l);

                if (!*p)
                        continue;

                if (strchr(COMMENTS, *p))
                        continue;

                if ((k = apply_rule(p)) < 0 && r == 0)
                        r = k;
        }

finish:
        fclose(f);

        return r;
}

static int scandir_filter(const struct dirent *d) {
        assert(d);

        if (ignore_file(d->d_name))
                return 0;

        if (d->d_type != DT_REG &&
            d->d_type != DT_LNK &&
            d->d_type != DT_UNKNOWN)
                return 0;

        return endswith(d->d_name, ".conf");
}

static int apply_tree(const char *path) {
        struct dirent **de = NULL;
        int n, i, r = 0;

        if ((n = scandir(path, &de, scandir_filter, alphasort)) < 0) {

                if (errno == ENOENT)
                        return 0;

                log_error("Failed to enumerate %s files: %m", path);
                return -errno;
        }

        for (i = 0; i < n; i++) {
                char *fn;
                int k;

                k = asprintf(&fn, "%s/%s", path, de[i]->d_name);
                free(de[i]);

                if (k < 0) {
                        log_error("Failed to allocate file name.");

                        if (r == 0)
                                r = -ENOMEM;
                        continue;
                }

                if ((k = apply_file(fn, true)) < 0 && r == 0)
                        r = k;
        }

        free(de);

        return r;
}

int main(int argc, char *argv[]) {
        int r = 0;

        if (argc > 2) {
                log_error("This program expects one or no arguments.");
                return EXIT_FAILURE;
        }

        log_set_target(LOG_TARGET_AUTO);
        log_parse_environment();
        log_open();

        if (argc > 1)
                r = apply_file(argv[1], false);
        else {
                /* Flush out all rules */
                write_one_line_file("/proc/sys/fs/binfmt_misc/status", "-1");

                r = apply_tree("/etc/binfmt.d");
        }

        return r < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
