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

#include <dbus/dbus.h>

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <getopt.h>
#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/prctl.h>

#include "manager.h"
#include "log.h"
#include "mount-setup.h"
#include "hostname-setup.h"
#include "loopback-setup.h"
#include "kmod-setup.h"
#include "locale-setup.h"
#include "selinux-setup.h"
#include "load-fragment.h"
#include "fdset.h"
#include "special.h"
#include "conf-parser.h"
#include "bus-errors.h"
#include "missing.h"
#include "label.h"
#include "build.h"
#include "strv.h"

static enum {
        ACTION_RUN,
        ACTION_HELP,
        ACTION_TEST,
        ACTION_DUMP_CONFIGURATION_ITEMS,
        ACTION_DONE
} arg_action = ACTION_RUN;

static char *arg_default_unit = NULL;
static ManagerRunningAs arg_running_as = _MANAGER_RUNNING_AS_INVALID;

static bool arg_dump_core = true;
static bool arg_crash_shell = false;
static int arg_crash_chvt = -1;
static bool arg_confirm_spawn = false;
static bool arg_show_status = true;
#ifdef HAVE_SYSV_COMPAT
static bool arg_sysv_console = true;
#endif
static bool arg_mount_auto = true;
static bool arg_swap_auto = true;
static char *arg_console = NULL;
static char **arg_default_controllers = NULL;

static FILE* serialization = NULL;

static void nop_handler(int sig) {
}

_noreturn_ static void crash(int sig) {

        if (!arg_dump_core)
                log_error("Caught <%s>, not dumping core.", signal_to_string(sig));
        else {
                struct sigaction sa;
                pid_t pid;

                /* We want to wait for the core process, hence let's enable SIGCHLD */
                zero(sa);
                sa.sa_handler = nop_handler;
                sa.sa_flags = SA_NOCLDSTOP|SA_RESTART;
                assert_se(sigaction(SIGCHLD, &sa, NULL) == 0);

                if ((pid = fork()) < 0)
                        log_error("Caught <%s>, cannot fork for core dump: %s", signal_to_string(sig), strerror(errno));

                else if (pid == 0) {
                        struct rlimit rl;

                        /* Enable default signal handler for core dump */
                        zero(sa);
                        sa.sa_handler = SIG_DFL;
                        assert_se(sigaction(sig, &sa, NULL) == 0);

                        /* Don't limit the core dump size */
                        zero(rl);
                        rl.rlim_cur = RLIM_INFINITY;
                        rl.rlim_max = RLIM_INFINITY;
                        setrlimit(RLIMIT_CORE, &rl);

                        /* Just to be sure... */
                        assert_se(chdir("/") == 0);

                        /* Raise the signal again */
                        raise(sig);

                        assert_not_reached("We shouldn't be here...");
                        _exit(1);

                } else {
                        siginfo_t status;
                        int r;

                        /* Order things nicely. */
                        if ((r = wait_for_terminate(pid, &status)) < 0)
                                log_error("Caught <%s>, waitpid() failed: %s", signal_to_string(sig), strerror(-r));
                        else if (status.si_code != CLD_DUMPED)
                                log_error("Caught <%s>, core dump failed.", signal_to_string(sig));
                        else
                                log_error("Caught <%s>, dumped core as pid %lu.", signal_to_string(sig), (unsigned long) pid);
                }
        }

        if (arg_crash_chvt)
                chvt(arg_crash_chvt);

        if (arg_crash_shell) {
                struct sigaction sa;
                pid_t pid;

                log_info("Executing crash shell in 10s...");
                sleep(10);

                /* Let the kernel reap children for us */
                zero(sa);
                sa.sa_handler = SIG_IGN;
                sa.sa_flags = SA_NOCLDSTOP|SA_NOCLDWAIT|SA_RESTART;
                assert_se(sigaction(SIGCHLD, &sa, NULL) == 0);

                if ((pid = fork()) < 0)
                        log_error("Failed to fork off crash shell: %s", strerror(errno));
                else if (pid == 0) {
                        int fd, r;

                        if ((fd = acquire_terminal("/dev/console", false, true, true)) < 0)
                                log_error("Failed to acquire terminal: %s", strerror(-fd));
                        else if ((r = make_stdio(fd)) < 0)
                                log_error("Failed to duplicate terminal fd: %s", strerror(-r));

                        execl("/bin/sh", "/bin/sh", NULL);

                        log_error("execl() failed: %s", strerror(errno));
                        _exit(1);
                }

                log_info("Successfully spawned crash shall as pid %lu.", (unsigned long) pid);
        }

        log_info("Freezing execution.");
        freeze();
}

static void install_crash_handler(void) {
        struct sigaction sa;

        zero(sa);

        sa.sa_handler = crash;
        sa.sa_flags = SA_NODEFER;

        sigaction_many(&sa, SIGNALS_CRASH_HANDLER, -1);
}

static int console_setup(bool do_reset) {
        int tty_fd, r;

        /* If we are init, we connect stdin/stdout/stderr to /dev/null
         * and make sure we don't have a controlling tty. */

        release_terminal();

        if (!do_reset)
                return 0;

        if ((tty_fd = open_terminal("/dev/console", O_WRONLY|O_NOCTTY|O_CLOEXEC)) < 0) {
                log_error("Failed to open /dev/console: %s", strerror(-tty_fd));
                return -tty_fd;
        }

        if ((r = reset_terminal(tty_fd)) < 0)
                log_error("Failed to reset /dev/console: %s", strerror(-r));

        close_nointr_nofail(tty_fd);
        return r;
}

static int set_default_unit(const char *u) {
        char *c;

        assert(u);

        if (!(c = strdup(u)))
                return -ENOMEM;

        free(arg_default_unit);
        arg_default_unit = c;
        return 0;
}

static int parse_proc_cmdline_word(const char *word) {

        static const char * const rlmap[] = {
                "emergency", SPECIAL_EMERGENCY_TARGET,
                "single",    SPECIAL_RESCUE_TARGET,
                "-s",        SPECIAL_RESCUE_TARGET,
                "s",         SPECIAL_RESCUE_TARGET,
                "S",         SPECIAL_RESCUE_TARGET,
                "1",         SPECIAL_RESCUE_TARGET,
                "2",         SPECIAL_RUNLEVEL2_TARGET,
                "3",         SPECIAL_RUNLEVEL3_TARGET,
                "4",         SPECIAL_RUNLEVEL4_TARGET,
                "5",         SPECIAL_RUNLEVEL5_TARGET,
        };

        assert(word);

        if (startswith(word, "systemd.unit="))
                return set_default_unit(word + 13);

        else if (startswith(word, "systemd.log_target=")) {

                if (log_set_target_from_string(word + 19) < 0)
                        log_warning("Failed to parse log target %s. Ignoring.", word + 19);

        } else if (startswith(word, "systemd.log_level=")) {

                if (log_set_max_level_from_string(word + 18) < 0)
                        log_warning("Failed to parse log level %s. Ignoring.", word + 18);

        } else if (startswith(word, "systemd.log_color=")) {

                if (log_show_color_from_string(word + 18) < 0)
                        log_warning("Failed to parse log color setting %s. Ignoring.", word + 18);

        } else if (startswith(word, "systemd.log_location=")) {

                if (log_show_location_from_string(word + 21) < 0)
                        log_warning("Failed to parse log location setting %s. Ignoring.", word + 21);

        } else if (startswith(word, "systemd.dump_core=")) {
                int r;

                if ((r = parse_boolean(word + 18)) < 0)
                        log_warning("Failed to parse dump core switch %s, Ignoring.", word + 18);
                else
                        arg_dump_core = r;

        } else if (startswith(word, "systemd.crash_shell=")) {
                int r;

                if ((r = parse_boolean(word + 20)) < 0)
                        log_warning("Failed to parse crash shell switch %s, Ignoring.", word + 20);
                else
                        arg_crash_shell = r;

        } else if (startswith(word, "systemd.confirm_spawn=")) {
                int r;

                if ((r = parse_boolean(word + 22)) < 0)
                        log_warning("Failed to parse confirm spawn switch %s, Ignoring.", word + 22);
                else
                        arg_confirm_spawn = r;

        } else if (startswith(word, "systemd.crash_chvt=")) {
                int k;

                if (safe_atoi(word + 19, &k) < 0)
                        log_warning("Failed to parse crash chvt switch %s, Ignoring.", word + 19);
                else
                        arg_crash_chvt = k;

        } else if (startswith(word, "systemd.show_status=")) {
                int r;

                if ((r = parse_boolean(word + 20)) < 0)
                        log_warning("Failed to parse show status switch %s, Ignoring.", word + 20);
                else
                        arg_show_status = r;
#ifdef HAVE_SYSV_COMPAT
        } else if (startswith(word, "systemd.sysv_console=")) {
                int r;

                if ((r = parse_boolean(word + 21)) < 0)
                        log_warning("Failed to parse SysV console switch %s, Ignoring.", word + 20);
                else
                        arg_sysv_console = r;
#endif

        } else if (startswith(word, "systemd.")) {

                log_warning("Unknown kernel switch %s. Ignoring.", word);

                log_info("Supported kernel switches:\n"
                         "systemd.unit=UNIT                        Default unit to start\n"
                         "systemd.dump_core=0|1                    Dump core on crash\n"
                         "systemd.crash_shell=0|1                  Run shell on crash\n"
                         "systemd.crash_chvt=N                     Change to VT #N on crash\n"
                         "systemd.confirm_spawn=0|1                Confirm every process spawn\n"
                         "systemd.show_status=0|1                  Show status updates on the console during bootup\n"
#ifdef HAVE_SYSV_COMPAT
                         "systemd.sysv_console=0|1                 Connect output of SysV scripts to console\n"
#endif
                         "systemd.log_target=console|kmsg|syslog|syslog-or-kmsg|null\n"
                         "                                         Log target\n"
                         "systemd.log_level=LEVEL                  Log level\n"
                         "systemd.log_color=0|1                    Highlight important log messages\n"
                         "systemd.log_location=0|1                 Include code location in log messages\n");

        } else if (startswith(word, "console=")) {
                const char *k;
                size_t l;
                char *w = NULL;

                k = word + 8;
                l = strcspn(k, ",");

                /* Ignore the console setting if set to a VT */
                if (l < 4 ||
                    !startswith(k, "tty") ||
                    k[3+strspn(k+3, "0123456789")] != 0) {

                        if (!(w = strndup(k, l)))
                                return -ENOMEM;
                }

                free(arg_console);
                arg_console = w;

        } else if (streq(word, "quiet")) {
                arg_show_status = false;
#ifdef HAVE_SYSV_COMPAT
                arg_sysv_console = false;
#endif
        } else {
                unsigned i;

                /* SysV compatibility */
                for (i = 0; i < ELEMENTSOF(rlmap); i += 2)
                        if (streq(word, rlmap[i]))
                                return set_default_unit(rlmap[i+1]);
        }

        return 0;
}

static int config_parse_level(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                const char *rvalue,
                void *data,
                void *userdata) {

        assert(filename);
        assert(lvalue);
        assert(rvalue);

        log_set_max_level_from_string(rvalue);
        return 0;
}

static int config_parse_target(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                const char *rvalue,
                void *data,
                void *userdata) {

        assert(filename);
        assert(lvalue);
        assert(rvalue);

        log_set_target_from_string(rvalue);
        return 0;
}

static int config_parse_color(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                const char *rvalue,
                void *data,
                void *userdata) {

        assert(filename);
        assert(lvalue);
        assert(rvalue);

        log_show_color_from_string(rvalue);
        return 0;
}

static int config_parse_location(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                const char *rvalue,
                void *data,
                void *userdata) {

        assert(filename);
        assert(lvalue);
        assert(rvalue);

        log_show_location_from_string(rvalue);
        return 0;
}

static int config_parse_cpu_affinity(
                const char *filename,
                unsigned line,
                const char *section,
                const char *lvalue,
                const char *rvalue,
                void *data,
                void *userdata) {

        char *w;
        size_t l;
        char *state;
        cpu_set_t *c = NULL;
        unsigned ncpus = 0;

        assert(filename);
        assert(lvalue);
        assert(rvalue);

        FOREACH_WORD_QUOTED(w, l, rvalue, state) {
                char *t;
                int r;
                unsigned cpu;

                if (!(t = strndup(w, l)))
                        return -ENOMEM;

                r = safe_atou(t, &cpu);
                free(t);

                if (!c)
                        if (!(c = cpu_set_malloc(&ncpus)))
                                return -ENOMEM;

                if (r < 0 || cpu >= ncpus) {
                        log_error("[%s:%u] Failed to parse CPU affinity: %s", filename, line, rvalue);
                        CPU_FREE(c);
                        return -EBADMSG;
                }

                CPU_SET_S(cpu, CPU_ALLOC_SIZE(ncpus), c);
        }

        if (c) {
                if (sched_setaffinity(0, CPU_ALLOC_SIZE(ncpus), c) < 0)
                        log_warning("Failed to set CPU affinity: %m");

                CPU_FREE(c);
        }

        return 0;
}

static int parse_config_file(void) {

        const ConfigItem items[] = {
                { "LogLevel",    config_parse_level,        NULL,               "Manager" },
                { "LogTarget",   config_parse_target,       NULL,               "Manager" },
                { "LogColor",    config_parse_color,        NULL,               "Manager" },
                { "LogLocation", config_parse_location,     NULL,               "Manager" },
                { "DumpCore",    config_parse_bool,         &arg_dump_core,     "Manager" },
                { "CrashShell",  config_parse_bool,         &arg_crash_shell,   "Manager" },
                { "ShowStatus",  config_parse_bool,         &arg_show_status,   "Manager" },
#ifdef HAVE_SYSV_COMPAT
                { "SysVConsole", config_parse_bool,         &arg_sysv_console,  "Manager" },
#endif
                { "CrashChVT",   config_parse_int,          &arg_crash_chvt,    "Manager" },
                { "CPUAffinity", config_parse_cpu_affinity, NULL,               "Manager" },
                { "MountAuto",   config_parse_bool,         &arg_mount_auto,    "Manager" },
                { "SwapAuto",    config_parse_bool,         &arg_swap_auto,     "Manager" },
                { "DefaultControllers", config_parse_strv,  &arg_default_controllers, "Manager" },
                { NULL, NULL, NULL, NULL }
        };

        static const char * const sections[] = {
                "Manager",
                NULL
        };

        FILE *f;
        const char *fn;
        int r;

        fn = arg_running_as == MANAGER_SYSTEM ? SYSTEM_CONFIG_FILE : USER_CONFIG_FILE;

        if (!(f = fopen(fn, "re"))) {
                if (errno == ENOENT)
                        return 0;

                log_warning("Failed to open configuration file '%s': %m", fn);
                return 0;
        }

        if ((r = config_parse(fn, f, sections, items, false, NULL)) < 0)
                log_warning("Failed to parse configuration file: %s", strerror(-r));

        fclose(f);

        return 0;
}

static int parse_proc_cmdline(void) {
        char *line, *w, *state;
        int r;
        size_t l;

        if ((r = read_one_line_file("/proc/cmdline", &line)) < 0) {
                log_warning("Failed to read /proc/cmdline, ignoring: %s", strerror(-r));
                return 0;
        }

        FOREACH_WORD_QUOTED(w, l, line, state) {
                char *word;

                if (!(word = strndup(w, l))) {
                        r = -ENOMEM;
                        goto finish;
                }

                r = parse_proc_cmdline_word(word);
                free(word);

                if (r < 0)
                        goto finish;
        }

        r = 0;

finish:
        free(line);
        return r;
}

static int parse_argv(int argc, char *argv[]) {

        enum {
                ARG_LOG_LEVEL = 0x100,
                ARG_LOG_TARGET,
                ARG_LOG_COLOR,
                ARG_LOG_LOCATION,
                ARG_UNIT,
                ARG_SYSTEM,
                ARG_USER,
                ARG_TEST,
                ARG_DUMP_CONFIGURATION_ITEMS,
                ARG_DUMP_CORE,
                ARG_CRASH_SHELL,
                ARG_CONFIRM_SPAWN,
                ARG_SHOW_STATUS,
                ARG_SYSV_CONSOLE,
                ARG_DESERIALIZE,
                ARG_INTROSPECT
        };

        static const struct option options[] = {
                { "log-level",                required_argument, NULL, ARG_LOG_LEVEL                },
                { "log-target",               required_argument, NULL, ARG_LOG_TARGET               },
                { "log-color",                optional_argument, NULL, ARG_LOG_COLOR                },
                { "log-location",             optional_argument, NULL, ARG_LOG_LOCATION             },
                { "unit",                     required_argument, NULL, ARG_UNIT                     },
                { "system",                   no_argument,       NULL, ARG_SYSTEM                   },
                { "user",                     no_argument,       NULL, ARG_USER                     },
                { "test",                     no_argument,       NULL, ARG_TEST                     },
                { "help",                     no_argument,       NULL, 'h'                          },
                { "dump-configuration-items", no_argument,       NULL, ARG_DUMP_CONFIGURATION_ITEMS },
                { "dump-core",                no_argument,       NULL, ARG_DUMP_CORE                },
                { "crash-shell",              no_argument,       NULL, ARG_CRASH_SHELL              },
                { "confirm-spawn",            no_argument,       NULL, ARG_CONFIRM_SPAWN            },
                { "show-status",              optional_argument, NULL, ARG_SHOW_STATUS              },
#ifdef HAVE_SYSV_COMPAT
                { "sysv-console",             optional_argument, NULL, ARG_SYSV_CONSOLE             },
#endif
                { "deserialize",              required_argument, NULL, ARG_DESERIALIZE              },
                { "introspect",               optional_argument, NULL, ARG_INTROSPECT               },
                { NULL,                       0,                 NULL, 0                            }
        };

        int c, r;

        assert(argc >= 1);
        assert(argv);

        while ((c = getopt_long(argc, argv, "hD", options, NULL)) >= 0)

                switch (c) {

                case ARG_LOG_LEVEL:
                        if ((r = log_set_max_level_from_string(optarg)) < 0) {
                                log_error("Failed to parse log level %s.", optarg);
                                return r;
                        }

                        break;

                case ARG_LOG_TARGET:

                        if ((r = log_set_target_from_string(optarg)) < 0) {
                                log_error("Failed to parse log target %s.", optarg);
                                return r;
                        }

                        break;

                case ARG_LOG_COLOR:

                        if (optarg) {
                                if ((r = log_show_color_from_string(optarg)) < 0) {
                                        log_error("Failed to parse log color setting %s.", optarg);
                                        return r;
                                }
                        } else
                                log_show_color(true);

                        break;

                case ARG_LOG_LOCATION:

                        if (optarg) {
                                if ((r = log_show_location_from_string(optarg)) < 0) {
                                        log_error("Failed to parse log location setting %s.", optarg);
                                        return r;
                                }
                        } else
                                log_show_location(true);

                        break;

                case ARG_UNIT:

                        if ((r = set_default_unit(optarg)) < 0) {
                                log_error("Failed to set default unit %s: %s", optarg, strerror(-r));
                                return r;
                        }

                        break;

                case ARG_SYSTEM:
                        arg_running_as = MANAGER_SYSTEM;
                        break;

                case ARG_USER:
                        arg_running_as = MANAGER_USER;
                        break;

                case ARG_TEST:
                        arg_action = ACTION_TEST;
                        break;

                case ARG_DUMP_CONFIGURATION_ITEMS:
                        arg_action = ACTION_DUMP_CONFIGURATION_ITEMS;
                        break;

                case ARG_DUMP_CORE:
                        arg_dump_core = true;
                        break;

                case ARG_CRASH_SHELL:
                        arg_crash_shell = true;
                        break;

                case ARG_CONFIRM_SPAWN:
                        arg_confirm_spawn = true;
                        break;

                case ARG_SHOW_STATUS:

                        if (optarg) {
                                if ((r = parse_boolean(optarg)) < 0) {
                                        log_error("Failed to show status boolean %s.", optarg);
                                        return r;
                                }
                                arg_show_status = r;
                        } else
                                arg_show_status = true;
                        break;
#ifdef HAVE_SYSV_COMPAT
                case ARG_SYSV_CONSOLE:

                        if (optarg) {
                                if ((r = parse_boolean(optarg)) < 0) {
                                        log_error("Failed to SysV console boolean %s.", optarg);
                                        return r;
                                }
                                arg_sysv_console = r;
                        } else
                                arg_sysv_console = true;
                        break;
#endif

                case ARG_DESERIALIZE: {
                        int fd;
                        FILE *f;

                        if ((r = safe_atoi(optarg, &fd)) < 0 || fd < 0) {
                                log_error("Failed to parse deserialize option %s.", optarg);
                                return r;
                        }

                        if (!(f = fdopen(fd, "r"))) {
                                log_error("Failed to open serialization fd: %m");
                                return r;
                        }

                        if (serialization)
                                fclose(serialization);

                        serialization = f;

                        break;
                }

                case ARG_INTROSPECT: {
                        const char * const * i = NULL;

                        for (i = bus_interface_table; *i; i += 2)
                                if (!optarg || streq(i[0], optarg)) {
                                        fputs(DBUS_INTROSPECT_1_0_XML_DOCTYPE_DECL_NODE
                                              "<node>\n", stdout);
                                        fputs(i[1], stdout);
                                        fputs("</node>\n", stdout);

                                        if (optarg)
                                                break;
                                }

                        if (!i[0] && optarg)
                                log_error("Unknown interface %s.", optarg);

                        arg_action = ACTION_DONE;
                        break;
                }

                case 'h':
                        arg_action = ACTION_HELP;
                        break;

                case 'D':
                        log_set_max_level(LOG_DEBUG);
                        break;

                case '?':
                        return -EINVAL;

                default:
                        log_error("Unknown option code %c", c);
                        return -EINVAL;
                }

        /* PID 1 will get the kernel arguments as parameters, which we
         * ignore and unconditionally read from
         * /proc/cmdline. However, we need to ignore those arguments
         * here. */
        if (arg_running_as != MANAGER_SYSTEM && optind < argc) {
                log_error("Excess arguments.");
                return -EINVAL;
        }

        return 0;
}

static int help(void) {

        printf("%s [OPTIONS...]\n\n"
               "Starts up and maintains the system or user services.\n\n"
               "  -h --help                      Show this help\n"
               "     --test                      Determine startup sequence, dump it and exit\n"
               "     --dump-configuration-items  Dump understood unit configuration items\n"
               "     --introspect[=INTERFACE]    Extract D-Bus interface data\n"
               "     --unit=UNIT                 Set default unit\n"
               "     --system                    Run a system instance, even if PID != 1\n"
               "     --user                      Run a user instance\n"
               "     --dump-core                 Dump core on crash\n"
               "     --crash-shell               Run shell on crash\n"
               "     --confirm-spawn             Ask for confirmation when spawning processes\n"
               "     --show-status[=0|1]         Show status updates on the console during bootup\n"
#ifdef HAVE_SYSV_COMPAT
               "     --sysv-console[=0|1]        Connect output of SysV scripts to console\n"
#endif
               "     --log-target=TARGET         Set log target (console, syslog, kmsg, syslog-or-kmsg, null)\n"
               "     --log-level=LEVEL           Set log level (debug, info, notice, warning, err, crit, alert, emerg)\n"
               "     --log-color[=0|1]           Highlight important log messages\n"
               "     --log-location[=0|1]        Include code location in log messages\n",
               program_invocation_short_name);

        return 0;
}

static int prepare_reexecute(Manager *m, FILE **_f, FDSet **_fds) {
        FILE *f = NULL;
        FDSet *fds = NULL;
        int r;

        assert(m);
        assert(_f);
        assert(_fds);

        if ((r = manager_open_serialization(m, &f)) < 0) {
                log_error("Failed to create serialization faile: %s", strerror(-r));
                goto fail;
        }

        if (!(fds = fdset_new())) {
                r = -ENOMEM;
                log_error("Failed to allocate fd set: %s", strerror(-r));
                goto fail;
        }

        if ((r = manager_serialize(m, f, fds)) < 0) {
                log_error("Failed to serialize state: %s", strerror(-r));
                goto fail;
        }

        if (fseeko(f, 0, SEEK_SET) < 0) {
                log_error("Failed to rewind serialization fd: %m");
                goto fail;
        }

        if ((r = fd_cloexec(fileno(f), false)) < 0) {
                log_error("Failed to disable O_CLOEXEC for serialization: %s", strerror(-r));
                goto fail;
        }

        if ((r = fdset_cloexec(fds, false)) < 0) {
                log_error("Failed to disable O_CLOEXEC for serialization fds: %s", strerror(-r));
                goto fail;
        }

        *_f = f;
        *_fds = fds;

        return 0;

fail:
        fdset_free(fds);

        if (f)
                fclose(f);

        return r;
}

static struct dual_timestamp* parse_initrd_timestamp(struct dual_timestamp *t) {
        const char *e;
        unsigned long long a, b;

        assert(t);

        if (!(e = getenv("RD_TIMESTAMP")))
                return NULL;

        if (sscanf(e, "%llu %llu", &a, &b) != 2)
                return NULL;

        t->realtime = (usec_t) a;
        t->monotonic = (usec_t) b;

        return t;
}

static void test_mtab(void) {
        char *p;

        if (readlink_malloc("/etc/mtab", &p) >= 0) {
                bool b;

                b = streq(p, "/proc/self/mounts");
                free(p);

                if (b)
                        return;
        }

        log_error("/etc/mtab is not a symlink or not pointing to /proc/self/mounts. "
                  "This is not supported anymore. "
                  "Please make sure to replace this file by a symlink to avoid incorrect or misleading mount(8) output.");
}

int main(int argc, char *argv[]) {
        Manager *m = NULL;
        int r, retval = EXIT_FAILURE;
        FDSet *fds = NULL;
        bool reexecute = false;
        const char *shutdown_verb = NULL;
        dual_timestamp initrd_timestamp = { 0ULL, 0ULL };
        char systemd[] = "systemd";

        if (getpid() != 1 && strstr(program_invocation_short_name, "init")) {
                /* This is compatbility support for SysV, where
                 * calling init as a user is identical to telinit. */

                errno = -ENOENT;
                execv(SYSTEMCTL_BINARY_PATH, argv);
                log_error("Failed to exec " SYSTEMCTL_BINARY_PATH ": %m");
                return 1;
        }

        /* If we get started via the /sbin/init symlink then we are
           called 'init'. After a subsequent reexecution we are then
           called 'systemd'. That is confusing, hence let's call us
           systemd right-away. */

        program_invocation_short_name = systemd;
        prctl(PR_SET_NAME, systemd);

        log_show_color(isatty(STDERR_FILENO) > 0);
        log_show_location(false);
        log_set_max_level(LOG_INFO);

        if (getpid() == 1) {
                arg_running_as = MANAGER_SYSTEM;
                log_set_target(LOG_TARGET_SYSLOG_OR_KMSG);

                /* This might actually not return, but cause a
                 * reexecution */
                if (selinux_setup(argv) < 0)
                        goto finish;

                if (label_init() < 0)
                        goto finish;
        } else {
                arg_running_as = MANAGER_USER;
                log_set_target(LOG_TARGET_CONSOLE);
        }

        if (set_default_unit(SPECIAL_DEFAULT_TARGET) < 0)
                goto finish;

        /* Mount /proc, /sys and friends, so that /proc/cmdline and
         * /proc/$PID/fd is available. */
        if (geteuid() == 0 && !getenv("SYSTEMD_SKIP_API_MOUNTS"))
                if (mount_setup() < 0)
                        goto finish;

        /* Reset all signal handlers. */
        assert_se(reset_all_signal_handlers() == 0);

        /* If we are init, we can block sigkill. Yay. */
        ignore_signals(SIGNALS_IGNORE, -1);

        if (parse_config_file() < 0)
                goto finish;

        if (arg_running_as == MANAGER_SYSTEM)
                if (parse_proc_cmdline() < 0)
                        goto finish;

        log_parse_environment();

        if (parse_argv(argc, argv) < 0)
                goto finish;

        if (arg_action == ACTION_HELP) {
                retval = help();
                goto finish;
        } else if (arg_action == ACTION_DUMP_CONFIGURATION_ITEMS) {
                unit_dump_config_items(stdout);
                retval = EXIT_SUCCESS;
                goto finish;
        } else if (arg_action == ACTION_DONE) {
                retval = EXIT_SUCCESS;
                goto finish;
        }

        assert_se(arg_action == ACTION_RUN || arg_action == ACTION_TEST);

        /* Remember open file descriptors for later deserialization */
        if (serialization) {
                if ((r = fdset_new_fill(&fds)) < 0) {
                        log_error("Failed to allocate fd set: %s", strerror(-r));
                        goto finish;
                }

                assert_se(fdset_remove(fds, fileno(serialization)) >= 0);
        } else
                close_all_fds(NULL, 0);

        /* Set up PATH unless it is already set */
        setenv("PATH",
               "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
               arg_running_as == MANAGER_SYSTEM);

        if (arg_running_as == MANAGER_SYSTEM) {
                /* Parse the data passed to us by the initrd and unset it */
                parse_initrd_timestamp(&initrd_timestamp);
                filter_environ("RD_");

                /* Unset some environment variables passed in from the
                 * kernel that don't really make sense for us. */
                unsetenv("HOME");
                unsetenv("TERM");
        }

        /* Move out of the way, so that we won't block unmounts */
        assert_se(chdir("/")  == 0);

        if (arg_running_as == MANAGER_SYSTEM) {
                /* Become a session leader if we aren't one yet. */
                setsid();

                /* Disable the umask logic */
                umask(0);
        }

        /* Make sure D-Bus doesn't fiddle with the SIGPIPE handlers */
        dbus_connection_set_change_sigpipe(FALSE);

        /* Reset the console, but only if this is really init and we
         * are freshly booted */
        if (arg_running_as == MANAGER_SYSTEM && arg_action == ACTION_RUN) {
                console_setup(getpid() == 1 && !serialization);
                make_null_stdio();
        }

        /* Open the logging devices, if possible and necessary */
        log_open();

        /* Make sure we leave a core dump without panicing the
         * kernel. */
        if (getpid() == 1)
                install_crash_handler();

        log_full(arg_running_as == MANAGER_SYSTEM ? LOG_INFO : LOG_DEBUG,
                 PACKAGE_STRING " running in %s mode. (" SYSTEMD_FEATURES "; " DISTRIBUTION ")", manager_running_as_to_string(arg_running_as));

        if (arg_running_as == MANAGER_SYSTEM && !serialization) {
                locale_setup();

                if (arg_show_status)
                        status_welcome();

                kmod_setup();
                hostname_setup();
                loopback_setup();

                mkdir_p("/dev/.systemd/ask-password/", 0755);

                test_mtab();
        }

        if ((r = manager_new(arg_running_as, &m)) < 0) {
                log_error("Failed to allocate manager object: %s", strerror(-r));
                goto finish;
        }

        m->confirm_spawn = arg_confirm_spawn;
        m->show_status = arg_show_status;
#ifdef HAVE_SYSV_COMPAT
        m->sysv_console = arg_sysv_console;
#endif
        m->mount_auto = arg_mount_auto;
        m->swap_auto = arg_swap_auto;

        if (dual_timestamp_is_set(&initrd_timestamp))
                m->initrd_timestamp = initrd_timestamp;

        if (arg_console)
                manager_set_console(m, arg_console);

        if (arg_default_controllers)
                manager_set_default_controllers(m, arg_default_controllers);

        if ((r = manager_startup(m, serialization, fds)) < 0)
                log_error("Failed to fully start up daemon: %s", strerror(-r));

        if (fds) {
                /* This will close all file descriptors that were opened, but
                 * not claimed by any unit. */

                fdset_free(fds);
                fds = NULL;
        }

        if (serialization) {
                fclose(serialization);
                serialization = NULL;
        } else {
                DBusError error;
                Unit *target = NULL;

                dbus_error_init(&error);

                log_debug("Activating default unit: %s", arg_default_unit);

                if ((r = manager_load_unit(m, arg_default_unit, NULL, &error, &target)) < 0) {
                        log_error("Failed to load default target: %s", bus_error(&error, r));
                        dbus_error_free(&error);
                } else if (target->meta.load_state == UNIT_ERROR)
                        log_error("Failed to load default target: %s", strerror(-target->meta.load_error));
                else if (target->meta.load_state == UNIT_MASKED)
                        log_error("Default target masked.");

                if (!target || target->meta.load_state != UNIT_LOADED) {
                        log_info("Trying to load rescue target...");

                        if ((r = manager_load_unit(m, SPECIAL_RESCUE_TARGET, NULL, &error, &target)) < 0) {
                                log_error("Failed to load rescue target: %s", bus_error(&error, r));
                                dbus_error_free(&error);
                                goto finish;
                        } else if (target->meta.load_state == UNIT_ERROR) {
                                log_error("Failed to load rescue target: %s", strerror(-target->meta.load_error));
                                goto finish;
                        } else if (target->meta.load_state == UNIT_MASKED) {
                                log_error("Rescue target masked.");
                                goto finish;
                        }
                }

                assert(target->meta.load_state == UNIT_LOADED);

                if (arg_action == ACTION_TEST) {
                        printf("-> By units:\n");
                        manager_dump_units(m, stdout, "\t");
                }

                if ((r = manager_add_job(m, JOB_START, target, JOB_REPLACE, false, &error, NULL)) < 0) {
                        log_error("Failed to start default target: %s", bus_error(&error, r));
                        dbus_error_free(&error);
                        goto finish;
                }

                if (arg_action == ACTION_TEST) {
                        printf("-> By jobs:\n");
                        manager_dump_jobs(m, stdout, "\t");
                        retval = EXIT_SUCCESS;
                        goto finish;
                }
        }

        for (;;) {
                if ((r = manager_loop(m)) < 0) {
                        log_error("Failed to run mainloop: %s", strerror(-r));
                        goto finish;
                }

                switch (m->exit_code) {

                case MANAGER_EXIT:
                        retval = EXIT_SUCCESS;
                        log_debug("Exit.");
                        goto finish;

                case MANAGER_RELOAD:
                        log_info("Reloading.");
                        if ((r = manager_reload(m)) < 0)
                                log_error("Failed to reload: %s", strerror(-r));
                        break;

                case MANAGER_REEXECUTE:
                        if (prepare_reexecute(m, &serialization, &fds) < 0)
                                goto finish;

                        reexecute = true;
                        log_notice("Reexecuting.");
                        goto finish;

                case MANAGER_REBOOT:
                case MANAGER_POWEROFF:
                case MANAGER_HALT:
                case MANAGER_KEXEC: {
                        static const char * const table[_MANAGER_EXIT_CODE_MAX] = {
                                [MANAGER_REBOOT] = "reboot",
                                [MANAGER_POWEROFF] = "poweroff",
                                [MANAGER_HALT] = "halt",
                                [MANAGER_KEXEC] = "kexec"
                        };

                        assert_se(shutdown_verb = table[m->exit_code]);

                        log_notice("Shutting down.");
                        goto finish;
                }

                default:
                        assert_not_reached("Unknown exit code.");
                }
        }

finish:
        if (m)
                manager_free(m);

        free(arg_default_unit);
        free(arg_console);
        strv_free(arg_default_controllers);

        dbus_shutdown();

        label_finish();

        if (reexecute) {
                const char *args[15];
                unsigned i = 0;
                char sfd[16];

                assert(serialization);
                assert(fds);

                args[i++] = SYSTEMD_BINARY_PATH;

                args[i++] = "--log-level";
                args[i++] = log_level_to_string(log_get_max_level());

                args[i++] = "--log-target";
                args[i++] = log_target_to_string(log_get_target());

                if (arg_running_as == MANAGER_SYSTEM)
                        args[i++] = "--system";
                else
                        args[i++] = "--user";

                if (arg_dump_core)
                        args[i++] = "--dump-core";

                if (arg_crash_shell)
                        args[i++] = "--crash-shell";

                if (arg_confirm_spawn)
                        args[i++] = "--confirm-spawn";

                if (arg_show_status)
                        args[i++] = "--show-status=1";
                else
                        args[i++] = "--show-status=0";

#ifdef HAVE_SYSV_COMPAT
                if (arg_sysv_console)
                        args[i++] = "--sysv-console=1";
                else
                        args[i++] = "--sysv-console=0";
#endif

                snprintf(sfd, sizeof(sfd), "%i", fileno(serialization));
                char_array_0(sfd);

                args[i++] = "--deserialize";
                args[i++] = sfd;

                args[i++] = NULL;

                assert(i <= ELEMENTSOF(args));

                execv(args[0], (char* const*) args);

                log_error("Failed to reexecute: %m");
        }

        if (serialization)
                fclose(serialization);

        if (fds)
                fdset_free(fds);

        if (shutdown_verb) {
                const char * command_line[] = {
                        SYSTEMD_SHUTDOWN_BINARY_PATH,
                        shutdown_verb,
                        NULL
                };

                execv(SYSTEMD_SHUTDOWN_BINARY_PATH, (char **) command_line);
                log_error("Failed to execute shutdown binary, freezing: %m");
        }

        if (getpid() == 1)
                freeze();

        return retval;
}
