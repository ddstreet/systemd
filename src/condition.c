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
#include <errno.h>
#include <string.h>
#include <unistd.h>

#ifdef HAVE_SELINUX
#include <selinux/selinux.h>
#endif

#include "util.h"
#include "condition.h"

Condition* condition_new(ConditionType type, const char *parameter, bool trigger, bool negate) {
        Condition *c;

        if (!(c = new0(Condition, 1)))
                return NULL;

        c->type = type;
        c->trigger = trigger;
        c->negate = negate;

        if (parameter)
                if (!(c->parameter = strdup(parameter))) {
                        free(c);
                        return NULL;
                }

        return c;
}

void condition_free(Condition *c) {
        assert(c);

        free(c->parameter);
        free(c);
}

void condition_free_list(Condition *first) {
        Condition *c, *n;

        LIST_FOREACH_SAFE(conditions, c, n, first)
                condition_free(c);
}

static bool test_kernel_command_line(const char *parameter) {
        char *line, *w, *state, *word = NULL;
        bool equal;
        int r;
        size_t l, pl;
        bool found = false;

        assert(parameter);

        if (detect_virtualization(NULL) > 0)
                return false;

        if ((r = read_one_line_file("/proc/cmdline", &line)) < 0) {
                log_warning("Failed to read /proc/cmdline, ignoring: %s", strerror(-r));
                return false;
        }

        equal = !!strchr(parameter, '=');
        pl = strlen(parameter);

        FOREACH_WORD_QUOTED(w, l, line, state) {

                free(word);
                if (!(word = strndup(w, l)))
                        break;

                if (equal) {
                        if (streq(word, parameter)) {
                                found = true;
                                break;
                        }
                } else {
                        if (startswith(word, parameter) && (word[pl] == '=' || word[pl] == 0)) {
                                found = true;
                                break;
                        }
                }

        }

        free(word);
        free(line);

        return found;
}

static bool test_virtualization(const char *parameter) {
        int r, b;
        const char *id;

        assert(parameter);

        if ((r = detect_virtualization(&id)) < 0) {
                log_warning("Failed to detect virtualization, ignoring: %s", strerror(-r));
                return false;
        }

        b = parse_boolean(parameter);

        if (r > 0 && b > 0)
                return true;

        if (r == 0 && b == 0)
                return true;

        return streq(parameter, id);
}

static bool test_security(const char *parameter) {
#ifdef HAVE_SELINUX
        if (streq(parameter, "selinux"))
                return is_selinux_enabled() > 0;
#endif
        return false;
}

bool condition_test(Condition *c) {
        assert(c);

        switch(c->type) {

        case CONDITION_PATH_EXISTS:
                return (access(c->parameter, F_OK) >= 0) == !c->negate;

        case CONDITION_PATH_IS_DIRECTORY: {
                struct stat st;

                if (lstat(c->parameter, &st) < 0)
                        return !c->negate;
                return S_ISDIR(st.st_mode) == !c->negate;
        }

        case CONDITION_DIRECTORY_NOT_EMPTY: {
                int k;

                k = dir_is_empty(c->parameter);
                return !(k == -ENOENT || k > 0) == !c->negate;
        }

        case CONDITION_KERNEL_COMMAND_LINE:
                return test_kernel_command_line(c->parameter) == !c->negate;

        case CONDITION_VIRTUALIZATION:
                return test_virtualization(c->parameter) == !c->negate;

        case CONDITION_SECURITY:
                return test_security(c->parameter) == !c->negate;

        case CONDITION_NULL:
                return !c->negate;

        default:
                assert_not_reached("Invalid condition type.");
        }
}

bool condition_test_list(Condition *first) {
        Condition *c;
        int triggered = -1;

        /* If the condition list is empty, then it is true */
        if (!first)
                return true;

        /* Otherwise, if all of the non-trigger conditions apply and
         * if any of the trigger conditions apply (unless there are
         * none) we return true */
        LIST_FOREACH(conditions, c, first) {
                bool b;

                b = condition_test(c);

                if (!c->trigger && !b)
                        return false;

                if (c->trigger && triggered <= 0)
                        triggered = b;
        }

        return triggered != 0;
}

void condition_dump(Condition *c, FILE *f, const char *prefix) {
        assert(c);
        assert(f);

        if (!prefix)
                prefix = "";

        fprintf(f,
                "%s\t%s: %s%s%s\n",
                prefix,
                condition_type_to_string(c->type),
                c->trigger ? "|" : "",
                c->negate ? "!" : "",
                c->parameter);
}

void condition_dump_list(Condition *first, FILE *f, const char *prefix) {
        Condition *c;

        LIST_FOREACH(conditions, c, first)
                condition_dump(c, f, prefix);
}

static const char* const condition_type_table[_CONDITION_TYPE_MAX] = {
        [CONDITION_PATH_EXISTS] = "ConditionPathExists",
        [CONDITION_PATH_IS_DIRECTORY] = "ConditionPathIsDirectory",
        [CONDITION_DIRECTORY_NOT_EMPTY] = "ConditionDirectoryNotEmpty",
        [CONDITION_KERNEL_COMMAND_LINE] = "ConditionKernelCommandLine",
        [CONDITION_VIRTUALIZATION] = "ConditionVirtualization",
        [CONDITION_SECURITY] = "ConditionSecurity",
        [CONDITION_NULL] = "ConditionNull"
};

DEFINE_STRING_TABLE_LOOKUP(condition_type, ConditionType);
