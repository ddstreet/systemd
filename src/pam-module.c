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

#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <pwd.h>
#include <endian.h>

#include <security/pam_modules.h>
#include <security/_pam_macros.h>
#include <security/pam_modutil.h>
#include <security/pam_ext.h>
#include <security/pam_misc.h>

#include "util.h"
#include "cgroup-util.h"
#include "macro.h"
#include "sd-daemon.h"
#include "strv.h"

static int parse_argv(pam_handle_t *handle,
                      int argc, const char **argv,
                      bool *create_session,
                      bool *kill_session,
                      bool *kill_user,
                      char ***controllers) {

        unsigned i;
        bool controller_set = false;

        assert(argc >= 0);
        assert(argc == 0 || argv);

        for (i = 0; i < (unsigned) argc; i++) {
                int k;

                if (startswith(argv[i], "create-session=")) {
                        if ((k = parse_boolean(argv[i] + 15)) < 0) {
                                pam_syslog(handle, LOG_ERR, "Failed to parse create-session= argument.");
                                return k;
                        }

                        if (create_session)
                                *create_session = k;

                } else if (startswith(argv[i], "kill-session=")) {
                        if ((k = parse_boolean(argv[i] + 13)) < 0) {
                                pam_syslog(handle, LOG_ERR, "Failed to parse kill-session= argument.");
                                return k;
                        }

                        if (kill_session)
                                *kill_session = k;

                } else if (startswith(argv[i], "kill-user=")) {
                        if ((k = parse_boolean(argv[i] + 10)) < 0) {
                                pam_syslog(handle, LOG_ERR, "Failed to parse kill-user= argument.");
                                return k;
                        }

                        if (kill_user)
                                *kill_user = k;

                } else if (startswith(argv[i], "controllers=")) {

                        if (controllers) {
                                char **l;

                                if (!(l = strv_split(argv[i] + 12, ","))) {
                                        pam_syslog(handle, LOG_ERR, "Out of memory.");
                                        return -ENOMEM;
                                }

                                strv_free(*controllers);
                                *controllers = l;
                        }

                        controller_set = true;

                } else {
                        pam_syslog(handle, LOG_ERR, "Unknown parameter '%s'.", argv[i]);
                        return -EINVAL;
                }
        }

#if 0
        if (!controller_set && controllers) {
                char **l;

                if (!(l = strv_new("cpu", NULL))) {
                        pam_syslog(handle, LOG_ERR, "Out of memory");
                        return -ENOMEM;
                }

                *controllers = l;
        }
#endif

        if (controllers)
                strv_remove(*controllers, "name=systemd");

        if (kill_session && *kill_session && kill_user)
                *kill_user = true;

        return 0;
}

static int open_file_and_lock(const char *fn) {
        int fd;

        assert(fn);

        if ((fd = open(fn, O_RDWR|O_CLOEXEC|O_NOCTTY|O_NOFOLLOW|O_CREAT, 0600)) < 0)
                return -errno;

        /* The BSD socket semantics are a lot nicer than those of
         * POSIX locks. Which is why we use flock() here. BSD locking
         * does not work across NFS which however is not needed here
         * as the filesystems in question should be local, and only
         * locally accessible, and most likely even tmpfs. */

        if (flock(fd, LOCK_EX) < 0)
                return -errno;

        return fd;
}

enum {
        SESSION_ID_AUDIT = 'a',
        SESSION_ID_COUNTER = 'c',
        SESSION_ID_RANDOM = 'r'
};

static uint64_t get_session_id(int *mode) {
        char *s;
        int fd;

        assert(mode);

        /* First attempt: let's use the session ID of the audit
         * system, if it is available. */
        if (read_one_line_file("/proc/self/sessionid", &s) >= 0) {
                uint32_t u;
                int r;

                r = safe_atou32(s, &u);
                free(s);

                if (r >= 0 && u != (uint32_t) -1 && u > 0) {
                        *mode = SESSION_ID_AUDIT;
                        return (uint64_t) u;
                }
        }

        /* Second attempt, use our own counter. */
        if ((fd = open_file_and_lock(RUNTIME_DIR "/user/.pam-systemd-session")) >= 0) {
                uint64_t counter;
                ssize_t r;

                /* We do a bit of endianess swapping here, just to be
                 * sure. /var should be machine specific anyway, and
                 * /var/run even mounted from tmpfs, so this
                 * byteswapping should really not be necessary. But
                 * then again, you never know, so let's avoid any
                 * risk. */

                if (loop_read(fd, &counter, sizeof(counter), false) != sizeof(counter))
                        counter = 1;
                else
                        counter = le64toh(counter) + 1;

                if (lseek(fd, 0, SEEK_SET) == 0) {
                        uint64_t swapped = htole64(counter);

                        r = loop_write(fd, &swapped, sizeof(swapped), false);

                        if (r != sizeof(swapped))
                                r = -EIO;
                } else
                        r = -errno;

                close_nointr_nofail(fd);

                if (r >= 0) {
                        *mode = SESSION_ID_COUNTER;
                        return counter;
                }
        }

        *mode = SESSION_ID_RANDOM;

        /* Last attempt, pick a random value */
        return (uint64_t) random_ull();
}
static int get_user_data(
                pam_handle_t *handle,
                const char **ret_username,
                struct passwd **ret_pw) {

        const char *username = NULL;
        struct passwd *pw = NULL;
        int r;
        bool have_loginuid = false;
        char *s;

        assert(handle);
        assert(ret_username);
        assert(ret_pw);

        if (read_one_line_file("/proc/self/loginuid", &s) >= 0) {
                uint32_t u;

                r = safe_atou32(s, &u);
                free(s);

                if (r >= 0 && u != (uint32_t) -1 && u > 0) {
                        have_loginuid = true;
                        pw = pam_modutil_getpwuid(handle, u);
                }
        }

        if (!have_loginuid) {
                if ((r = pam_get_user(handle, &username, NULL)) != PAM_SUCCESS) {
                        pam_syslog(handle, LOG_ERR, "Failed to get user name.");
                        return r;
                }

                if (!username || !*username) {
                        pam_syslog(handle, LOG_ERR, "User name not valid.");
                        return PAM_AUTH_ERR;
                }

                pw = pam_modutil_getpwnam(handle, username);
        }

        if (!pw) {
                pam_syslog(handle, LOG_ERR, "Failed to get user data.");
                return PAM_USER_UNKNOWN;
        }

        *ret_pw = pw;
        *ret_username = username ? username : pw->pw_name;

        return PAM_SUCCESS;
}

static int create_user_group(
                pam_handle_t *handle,
                const char *controller,
                const char *group,
                struct passwd *pw,
                bool attach,
                bool remember) {

        int r;

        assert(handle);
        assert(group);

        if (attach)
                r = cg_create_and_attach(controller, group, 0);
        else
                r = cg_create(controller, group);

        if (r < 0) {
                pam_syslog(handle, LOG_ERR, "Failed to create cgroup: %s", strerror(-r));
                return PAM_SESSION_ERR;
        }

        if (r > 0 && remember) {
                /* Remember that it was us who created this group, and
                 * that hence we need to remove it too. This is a
                 * protection against removing the cgroup when run
                 * recursively. */
                if ((r = pam_set_data(handle, "systemd.created", INT_TO_PTR(1), NULL)) != PAM_SUCCESS) {
                        pam_syslog(handle, LOG_ERR, "Failed to install created variable.");
                        return r;
                }
        }

        if ((r = cg_set_task_access(controller, group, 0644, pw->pw_uid, pw->pw_gid)) < 0 ||
            (r = cg_set_group_access(controller, group, 0755, pw->pw_uid, pw->pw_gid)) < 0) {
                pam_syslog(handle, LOG_ERR, "Failed to change access modes: %s", strerror(-r));
                return PAM_SESSION_ERR;
        }

        return PAM_SUCCESS;
}

_public_ PAM_EXTERN int pam_sm_open_session(
                pam_handle_t *handle,
                int flags,
                int argc, const char **argv) {

        const char *username = NULL;
        struct passwd *pw;
        int r;
        char *buf = NULL;
        int lock_fd = -1;
        bool create_session = true;
        char **controllers = NULL, **c;

        assert(handle);

        /* pam_syslog(handle, LOG_DEBUG, "pam-systemd initializing"); */

        /* Make this a NOP on non-systemd systems */
        if (sd_booted() <= 0)
                return PAM_SUCCESS;

        if (parse_argv(handle, argc, argv, &create_session, NULL, NULL, &controllers) < 0)
                return PAM_SESSION_ERR;

        if ((r = get_user_data(handle, &username, &pw)) != PAM_SUCCESS)
                goto finish;

        if (safe_mkdir(RUNTIME_DIR "/user", 0755, 0, 0) < 0) {
                pam_syslog(handle, LOG_ERR, "Failed to create runtime directory: %m");
                r = PAM_SYSTEM_ERR;
                goto finish;
        }

        if ((lock_fd = open_file_and_lock(RUNTIME_DIR "/user/.pam-systemd-lock")) < 0) {
                pam_syslog(handle, LOG_ERR, "Failed to lock runtime directory: %m");
                r = PAM_SYSTEM_ERR;
                goto finish;
        }

        /* Create /var/run/$USER */
        free(buf);
        if (asprintf(&buf, RUNTIME_DIR "/user/%s", username) < 0) {
                r = PAM_BUF_ERR;
                goto finish;
        }

        if (safe_mkdir(buf, 0700, pw->pw_uid, pw->pw_gid) < 0) {
                pam_syslog(handle, LOG_WARNING, "Failed to create runtime directory: %m");
                r = PAM_SYSTEM_ERR;
                goto finish;
        } else if ((r = pam_misc_setenv(handle, "XDG_RUNTIME_DIR", buf, 0)) != PAM_SUCCESS) {
                pam_syslog(handle, LOG_ERR, "Failed to set runtime dir.");
                goto finish;
        }

        free(buf);
        buf = NULL;

        if (create_session) {
                const char *id;

                /* Reuse or create XDG session ID */
                if (!(id = pam_getenv(handle, "XDG_SESSION_ID"))) {
                        int mode;

                        if (asprintf(&buf, "%llux", (unsigned long long) get_session_id(&mode)) < 0) {
                                r = PAM_BUF_ERR;
                                goto finish;
                        }

                        /* To avoid id clashes we add the session id
                         * source to our session ids. Note that the
                         * session id source might change during
                         * runtime, because a filesystem became
                         * writable or the system reconfigured. */
                        buf[strlen(buf)-1] =
                                mode != SESSION_ID_AUDIT ? (char) mode : 0;

                        if ((r = pam_misc_setenv(handle, "XDG_SESSION_ID", buf, 0)) != PAM_SUCCESS) {
                                pam_syslog(handle, LOG_ERR, "Failed to set session id.");
                                goto finish;
                        }

                        if (!(id = pam_getenv(handle, "XDG_SESSION_ID"))) {
                                pam_syslog(handle, LOG_ERR, "Failed to get session id.");
                                r = PAM_SESSION_ERR;
                                goto finish;
                        }
                }

                r = asprintf(&buf, "/user/%s/%s", username, id);
        } else
                r = asprintf(&buf, "/user/%s/master", username);

        if (r < 0) {
                r = PAM_BUF_ERR;
                goto finish;
        }

        pam_syslog(handle, LOG_INFO, "Moving new user session for %s into control group %s.", username, buf);

        if ((r = create_user_group(handle, SYSTEMD_CGROUP_CONTROLLER, buf, pw, true, true)) != PAM_SUCCESS)
                goto finish;

        /* The additional controllers don't really matter, so we
         * ignore the return value */
        STRV_FOREACH(c, controllers)
                create_user_group(handle, *c, buf, pw, true, false);

        r = PAM_SUCCESS;

finish:
        free(buf);

        if (lock_fd >= 0)
                close_nointr_nofail(lock_fd);

        strv_free(controllers);

        return r;
}

static int session_remains(pam_handle_t *handle, const char *user_path) {
        int r;
        bool remains = false;
        DIR *d;
        char *subgroup;

        if ((r = cg_enumerate_subgroups(SYSTEMD_CGROUP_CONTROLLER, user_path, &d)) < 0)
                return r;

        while ((r = cg_read_subgroup(d, &subgroup)) > 0) {

                remains = !streq(subgroup, "master");
                free(subgroup);

                if (remains)
                        break;
        }

        closedir(d);

        if (r < 0)
                return r;

        return !!remains;
}

_public_ PAM_EXTERN int pam_sm_close_session(
                pam_handle_t *handle,
                int flags,
                int argc, const char **argv) {

        const char *username = NULL;
        bool kill_session = false;
        bool kill_user = false;
        int lock_fd = -1, r;
        char *session_path = NULL, *nosession_path = NULL, *user_path = NULL;
        const char *id;
        struct passwd *pw;
        const void *created = NULL;
        char **controllers = NULL, **c;

        assert(handle);

        /* Make this a NOP on non-systemd systems */
        if (sd_booted() <= 0)
                return PAM_SUCCESS;

        if (parse_argv(handle, argc, argv, NULL, &kill_session, &kill_user, &controllers) < 0)
                return PAM_SESSION_ERR;

        if ((r = get_user_data(handle, &username, &pw)) != PAM_SUCCESS)
                goto finish;

        if ((lock_fd = open_file_and_lock(RUNTIME_DIR "/user/.pam-systemd-lock")) < 0) {
                pam_syslog(handle, LOG_ERR, "Failed to lock runtime directory: %m");
                r = PAM_SYSTEM_ERR;
                goto finish;
        }

        /* We are probably still in some session/user dir. Move ourselves out of the way as first step */
        if ((r = cg_attach(SYSTEMD_CGROUP_CONTROLLER, "/user", 0)) < 0)
                pam_syslog(handle, LOG_ERR, "Failed to move us away: %s", strerror(-r));

        STRV_FOREACH(c, controllers)
                if ((r = cg_attach(*c, "/user", 0)) < 0)
                        pam_syslog(handle, LOG_ERR, "Failed to move us away in %s hierarchy: %s", *c, strerror(-r));

        if (asprintf(&user_path, "/user/%s", username) < 0) {
                r = PAM_BUF_ERR;
                goto finish;
        }

        pam_get_data(handle, "systemd.created", &created);

        if ((id = pam_getenv(handle, "XDG_SESSION_ID")) && created) {

                if (asprintf(&session_path, "/user/%s/%s", username, id) < 0 ||
                    asprintf(&nosession_path, "/user/%s/master", username) < 0) {
                        r = PAM_BUF_ERR;
                        goto finish;
                }

                if (kill_session)  {
                        pam_syslog(handle, LOG_INFO, "Killing remaining processes of user session %s of %s.", id, username);

                        /* Kill processes in session cgroup, and delete it */
                        if ((r = cg_kill_recursive_and_wait(SYSTEMD_CGROUP_CONTROLLER, session_path, true)) < 0)
                                pam_syslog(handle, LOG_ERR, "Failed to kill session cgroup: %s", strerror(-r));
                } else {
                        pam_syslog(handle, LOG_INFO, "Moving remaining processes of user session %s of %s into control group %s.", id, username, nosession_path);

                        /* Migrate processes from session to user
                         * cgroup. First, try to create the user group
                         * in case it doesn't exist yet. Also, delete
                         * the session group. */
                        create_user_group(handle, SYSTEMD_CGROUP_CONTROLLER, nosession_path, pw, false, false);

                        if ((r = cg_migrate_recursive(SYSTEMD_CGROUP_CONTROLLER, session_path, nosession_path, false, true)) < 0)
                                pam_syslog(handle, LOG_ERR, "Failed to migrate session cgroup: %s", strerror(-r));
                }

                STRV_FOREACH(c, controllers) {
                        create_user_group(handle, *c, nosession_path, pw, false, false);

                        if ((r = cg_migrate_recursive(*c, session_path, nosession_path, false, true)) < 0)
                                pam_syslog(handle, LOG_ERR, "Failed to migrate session cgroup in hierarchy %s: %s", *c, strerror(-r));
                }
        }

        /* GC user tree */
        cg_trim(SYSTEMD_CGROUP_CONTROLLER, user_path, false);

        if ((r = session_remains(handle, user_path)) < 0)
                pam_syslog(handle, LOG_ERR, "Failed to determine whether a session remains: %s", strerror(-r));

        /* Kill user processes not attached to any session */
        if (kill_user && r == 0) {

                /* Kill user cgroup */
                if ((r = cg_kill_recursive_and_wait(SYSTEMD_CGROUP_CONTROLLER, user_path, true)) < 0)
                        pam_syslog(handle, LOG_ERR, "Failed to kill user cgroup: %s", strerror(-r));
        } else {

                if ((r = cg_is_empty_recursive(SYSTEMD_CGROUP_CONTROLLER, user_path, true)) < 0)
                        pam_syslog(handle, LOG_ERR, "Failed to check user cgroup: %s", strerror(-r));

                /* Remove user cgroup */
                if (r > 0) {
                        if ((r = cg_delete(SYSTEMD_CGROUP_CONTROLLER, user_path)) < 0)
                                pam_syslog(handle, LOG_ERR, "Failed to delete user cgroup: %s", strerror(-r));

                /* If we managed to find somebody, don't cleanup the cgroup. */
                } else if (r == 0)
                        r = -EBUSY;
        }

        STRV_FOREACH(c, controllers)
                cg_trim(*c, user_path, true);

        if (r >= 0) {
                const char *runtime_dir;

                if ((runtime_dir = pam_getenv(handle, "XDG_RUNTIME_DIR")))
                        if ((r = rm_rf(runtime_dir, false, true)) < 0)
                                pam_syslog(handle, LOG_ERR, "Failed to remove runtime directory: %s", strerror(-r));
        }

        /* pam_syslog(handle, LOG_DEBUG, "pam-systemd done"); */

        r = PAM_SUCCESS;

finish:
        if (lock_fd >= 0)
                close_nointr_nofail(lock_fd);

        free(session_path);
        free(nosession_path);
        free(user_path);

        strv_free(controllers);

        return r;
}
