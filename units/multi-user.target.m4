#  This file is part of systemd.
#
#  systemd is free software; you can redistribute it and/or modify it
#  under the terms of the GNU General Public License as published by
#  the Free Software Foundation; either version 2 of the License, or
#  (at your option) any later version.

# See systemd.special(7) for details

[Unit]
Description=Multi-User
Requires=basic.target
Conflicts=rescue.target
After=basic.target rescue.target
m4_dnl
m4_ifdef(`TARGET_FEDORA',
m4_dnl On Fedora Runlevel 3 is multi-user
Names=runlevel3.target
)m4_dnl
m4_ifdef(`TARGET_SUSE',
Names=runlevel3.target
)m4_dnl
m4_ifdef(`TARGET_DEBIAN',
m4_dnl On Debian Runlevel 2, 3, 4 and 5 are multi-user
Names=runlevel2.target runlevel3.target runlevel4.target runlevel5.target
)m4_dnl
AllowIsolate=yes

[Install]
Alias=default.target
