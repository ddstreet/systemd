#! /bin/sh

# Copyright (C) 2010 Tollef Fog Heen <tfheen@err.no>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License version 2 as
# published by the Free Software Foundation
#
# This program is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
# or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
# for more details.
#
# You should have received a copy of the GNU General Public License along
# with this program; if not, write to the Free Software Foundation, Inc.,
# 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA.

set -e

QUIET=
FORCE=
TRY_ANYWAY=
DISCLOSE_DENY=
QUERY=
NO_FALLBACK=

usage() {
    cat <<EOF
invoke-rc.d, maintainer interface for systemd init scripts
EOF
    exit 1
}

if [ -z "$1" ] || [ "$1" = "--help" ]; then
   usage
fi

while [ "$#" -gt 1 ]; do
    case "$1" in
	--quiet)
	    QUIET=1
	    break
	    ;;
	--force)
	    FORCE=1
	    break
	    ;;
	--try-anyway)
	    TRY_ANYWAY=1
	    break
	    ;;
	--disclose-deny)
	    DISCLOSE_DENY=1
	    break
	    ;;
	--query)
	    QUERY=1
	    break
	    ;;
	--no-fallback)
	    NO_FALLBACK=1
	    break
	    ;;
	--*)
	    echo "Unknown option $1"
	    exit 1
	    ;;
	*)
	    
    esac
    shift
done