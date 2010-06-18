/*-*- Mode: C; c-basic-offset: 8 -*-*/

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

#include "log.h"

int main(int argc, char *argv[]) {
        DBusError error;
        DBusConnection *bus = NULL;
        DBusMessage *m = NULL;
        int r = 1;

        dbus_error_init(&error);

        if (argc != 2) {
                log_error("Incorrect number of arguments.");
                goto finish;
        }

        if (!(bus = dbus_bus_get(DBUS_BUS_SYSTEM, &error))) {
                log_error("Failed to get D-Bus connection: %s", error.message);
                goto finish;
        }

        if (!(m = dbus_message_new_signal("/org/freedesktop/systemd1/agent", "org.freedesktop.systemd1.Agent", "Released"))) {
                log_error("Could not allocate signal message.");
                goto finish;
        }

        if (!dbus_message_append_args(m,
                                      DBUS_TYPE_STRING, &argv[1],
                                      DBUS_TYPE_INVALID)) {
                log_error("Could not attach group information to signal message.");
                goto finish;
        }

        if (!dbus_connection_send(bus, m, NULL)) {
                log_error("Failed to send signal message.");
                goto finish;
        }

        r = 0;

finish:
        if (bus)
                dbus_connection_unref(bus);

        if (m)
                dbus_message_unref(m);

        dbus_error_free(&error);
        return r;
}
