/*-*- Mode: C; c-basic-offset: 8; indent-tabs-mode: nil -*-*/

#ifndef foodbushfoo
#define foodbushfoo

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

#ifndef DBUS_ERROR_UNKNOWN_OBJECT
#define DBUS_ERROR_UNKNOWN_OBJECT "org.freedesktop.DBus.Error.UnknownObject"
#endif

#ifndef DBUS_ERROR_UNKNOWN_INTERFACE
#define DBUS_ERROR_UNKNOWN_INTERFACE "org.freedesktop.DBus.Error.UnknownInterface"
#endif

#include "manager.h"

typedef int (*BusPropertyCallback)(Manager *m, DBusMessageIter *iter, const char *property, void *data);
typedef int (*BusPropertySetCallback)(Manager *m, DBusMessageIter *iter, const char *property);

typedef struct BusProperty {
        const char *interface;           /* interface of the property */
        const char *property;            /* name of the property */
        BusPropertyCallback append;      /* Function that is called to serialize this property */
        const char *signature;
        const void *data;                /* The data of this property */
        BusPropertySetCallback set;      /* Function that is called to set this property */
} BusProperty;

#define BUS_PROPERTIES_INTERFACE                                        \
        " <interface name=\"org.freedesktop.DBus.Properties\">\n"       \
        "  <method name=\"Get\">\n"                                     \
        "   <arg name=\"interface\" direction=\"in\" type=\"s\"/>\n"    \
        "   <arg name=\"property\" direction=\"in\" type=\"s\"/>\n"     \
        "   <arg name=\"value\" direction=\"out\" type=\"v\"/>\n"       \
        "  </method>\n"                                                 \
        "  <method name=\"GetAll\">\n"                                  \
        "   <arg name=\"interface\" direction=\"in\" type=\"s\"/>\n"    \
        "   <arg name=\"properties\" direction=\"out\" type=\"a{sv}\"/>\n" \
        "  </method>\n"                                                 \
        "  <method name=\"Set\">\n"                                     \
        "   <arg name=\"interface\" direction=\"in\" type=\"s\"/>\n"    \
        "   <arg name=\"property\" direction=\"in\" type=\"s\"/>\n"     \
        "   <arg name=\"value\" direction=\"in\" type=\"v\"/>\n"       \
        "  </method>\n"                                                 \
        "  <signal name=\"PropertiesChanged\">\n"                       \
        "   <arg type=\"s\" name=\"interface\"/>\n"                     \
        "   <arg type=\"a{sv}\" name=\"changed_properties\"/>\n"        \
        "   <arg type=\"as\" name=\"invalidated_properties\"/>\n"       \
        "  </signal>\n"                                                 \
        " </interface>\n"

#define BUS_INTROSPECTABLE_INTERFACE                                    \
        " <interface name=\"org.freedesktop.DBus.Introspectable\">\n"   \
        "  <method name=\"Introspect\">\n"                              \
        "   <arg name=\"data\" type=\"s\" direction=\"out\"/>\n"        \
        "  </method>\n"                                                 \
        " </interface>\n"

#define BUS_PEER_INTERFACE                                              \
        "<interface name=\"org.freedesktop.DBus.Peer\">\n"              \
        " <method name=\"Ping\"/>\n"                                    \
        " <method name=\"GetMachineId\">\n"                             \
        "  <arg type=\"s\" name=\"machine_uuid\" direction=\"out\"/>\n" \
        " </method>\n"                                                  \
        "</interface>\n"

int bus_init(Manager *m, bool try_bus_connect);
void bus_done(Manager *m);

unsigned bus_dispatch(Manager *m);

void bus_watch_event(Manager *m, Watch *w, int events);
void bus_timeout_event(Manager *m, Watch *w, int events);

int bus_query_pid(Manager *m, const char *name);

DBusHandlerResult bus_default_message_handler(Manager *m, DBusConnection *c, DBusMessage *message, const char* introspection, const BusProperty *properties);
DBusHandlerResult bus_send_error_reply(Manager *m, DBusConnection *c, DBusMessage *message, DBusError *bus_error, int error);

int bus_broadcast(Manager *m, DBusMessage *message);

int bus_property_append_string(Manager *m, DBusMessageIter *i, const char *property, void *data);
int bus_property_append_strv(Manager *m, DBusMessageIter *i, const char *property, void *data);
int bus_property_append_bool(Manager *m, DBusMessageIter *i, const char *property, void *data);
int bus_property_append_int32(Manager *m, DBusMessageIter *i, const char *property, void *data);
int bus_property_append_uint32(Manager *m, DBusMessageIter *i, const char *property, void *data);
int bus_property_append_uint64(Manager *m, DBusMessageIter *i, const char *property, void *data);
int bus_property_append_size(Manager *m, DBusMessageIter *i, const char *property, void *data);
int bus_property_append_ul(Manager *m, DBusMessageIter *i, const char *property, void *data);

#define bus_property_append_int bus_property_append_int32
#define bus_property_append_pid bus_property_append_uint32
#define bus_property_append_mode bus_property_append_uint32
#define bus_property_append_unsigned bus_property_append_uint32
#define bus_property_append_usec bus_property_append_uint64

#define DEFINE_BUS_PROPERTY_APPEND_ENUM(function,name,type)             \
        int function(Manager *m, DBusMessageIter *i, const char *property, void *data) { \
                const char *value;                                      \
                type *field = data;                                     \
                                                                        \
                assert(m);                                              \
                assert(i);                                              \
                assert(property);                                       \
                                                                        \
                value = name##_to_string(*field);                       \
                                                                        \
                if (!dbus_message_iter_append_basic(i, DBUS_TYPE_STRING, &value)) \
                        return -ENOMEM;                                 \
                                                                        \
                return 0;                                               \
        }

int bus_parse_strv(DBusMessage *m, char ***_l);

bool bus_has_subscriber(Manager *m);
bool bus_connection_has_subscriber(Manager *m, DBusConnection *c);

DBusMessage* bus_properties_changed_new(const char *path, const char *interface, const char *properties);

#define BUS_CONNECTION_SUBSCRIBED(m, c) dbus_connection_get_data((c), (m)->subscribed_data_slot)
#define BUS_PENDING_CALL_NAME(m, p) dbus_pending_call_get_data((p), (m)->name_data_slot)

extern const char * const bus_interface_table[];

#endif
