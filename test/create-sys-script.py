#!/usr/bin/python3

OUTFILE_HEADER = """#!/usr/bin/python3
#
# create-sys-script.py
#
# (C) 2017 Canonical Ltd.
# Author: Dan Streetman <dan.streetman@canonical.com>
#
# systemd is free software; you can redistribute it and/or modify it
# under the terms of the GNU Lesser General Public License as published by
# the Free Software Foundation; either version 2.1 of the License, or
# (at your option) any later version.
#
# systemd is distributed in the hope that it will be useful, but
# WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public License
# along with systemd; If not, see <http://www.gnu.org/licenses/>.
#
"""

# Use this only to (re-)create the test/sys-script.py script,
# after adding or modifying anything in the test/sys/ directory


import os, sys, stat, tempfile, filecmp


OUTFILE = "sys-script.py"
OUTFILE_MODE = 0o775

OUTFILE_FUNCS = r"""
import os, sys

def d(path, mode):
    os.mkdir(path, mode)

def l(path, src):
    os.symlink(src, path)

def f(path, mode, contents):
    # wrap string contents with b''''''
    s = "b'''{}'''".format(contents)
    # convert directly to bytes using eval
    b = eval(s)
    # write the bytes out to file
    outf = open(path, "wb")
    outf.write(b)
    outf.close()
    os.chmod(path, mode)

"""

OUTFILE_MAIN = """
if len(sys.argv) < 2:
    exit("Usage: {} <target dir>".format(sys.argv[0]))

if not os.path.isdir(sys.argv[1]):
    exit("Target dir {} not found".format(sys.argv[1]))

os.chdir(sys.argv[1])

"""


def handle_dir(outfile, path):
    m = os.lstat(path).st_mode & 0o777
    outfile.write("d('{}', 0o{:o})\n".format(path, m))


def handle_link(outfile, path):
    src = os.readlink(path)
    outfile.write("l('{}', '{}')\n".format(path, src))


def handle_file(outfile, path):
    m = os.lstat(path).st_mode & 0o777
    f = open(path, "rb")
    # read in as binary
    b = f.read()
    f.close()
    # convert to string containing b'' representation
    s = repr(b)
    # python is smart; it uses b"" if b contains only '.  Otherwise
    # (b has no quotes, or only ", or both ' and "), it uses
    # b'' and escapes all the ' (if any).
    q = s[1]
    # strip leading b' and trailing ' (or b"")
    s = s[2:-1]
    # b'' (or b"") contain newlines as \n, so convert back
    s = s.replace('\\n', '\n')
    # We're placing the string into r'''''' and any sequences of ''' or a
    # trailing ' would break that.  So escape ' only if needed; as explained
    # above, if the quote used is ' then all ' are already escaped, or there
    # are none - we only need to escape if the quote is "
    if q == '"':
        s = s.replace("'", r'\'')
    outfile.write("f('{}', 0o{:o}, r'''{}''')\n".format(path, m, s))


def process_sysdir(outfile):
    for (dirpath, dirnames, filenames) in os.walk("sys"):
        handle_dir(outfile, dirpath)
        for d in dirnames:
            path = os.path.join(dirpath, d)
            if stat.S_ISLNK(os.lstat(path).st_mode):
                handle_link(outfile, path)
        for f in filenames:
            path = os.path.join(dirpath, f)
            mode = os.lstat(path).st_mode
            if stat.S_ISLNK(mode):
                handle_link(outfile, path)
            elif stat.S_ISREG(mode):
                handle_file(outfile, path)


def verify_dir(tmpd, path_a):
    path_b = os.path.join(tmpd, path_a)
    mode_a = os.lstat(path_a).st_mode
    mode_b = os.lstat(path_b).st_mode
    if not stat.S_ISDIR(mode_b):
        raise Exception("Not directory")
    if (mode_a & 0o777) != (mode_b & 0o777):
        raise Exception("Permissions mismatch")


def verify_link(tmpd, path_a):
    path_b = os.path.join(tmpd, path_a)
    if not stat.S_ISLNK(os.lstat(path_b).st_mode):
        raise Exception("Not symlink")
    if os.readlink(path_a) != os.readlink(path_b):
        raise Exception("Symlink dest mismatch")


def verify_file(tmpd, path_a):
    path_b = os.path.join(tmpd, path_a)
    mode_a = os.lstat(path_a).st_mode
    mode_b = os.lstat(path_b).st_mode
    if not stat.S_ISREG(mode_b):
        raise Exception("Not file")
    if (mode_a & 0o777) != (mode_b & 0o777):
        raise Exception("Permissions mismatch")
    if not filecmp.cmp(path_a, path_b, shallow=False):
        raise Exception("File contents mismatch")


def verify_script(tmpd):
    path = ""
    try:
        for (dirpath, dirnames, filenames) in os.walk("sys"):
            path = dirpath
            verify_dir(tmpd, path)
            for d in dirnames:
                path = os.path.join(dirpath, d)
                if stat.S_ISLNK(os.lstat(path).st_mode):
                    verify_link(tmpd, path)
                for f in filenames:
                    path = os.path.join(dirpath, f)
                    mode = os.lstat(path).st_mode
                    if stat.S_ISLNK(mode):
                        verify_link(tmpd, path)
                    elif stat.S_ISREG(mode):
                        verify_file(tmpd, path)
    except Exception as e:
        print("FAIL on '{}'".format(path))
        raise(e)


if __name__ == "__main__":
    # Always operate in the dir where this script is
    os.chdir(os.path.dirname(sys.argv[0]))

    if not os.path.isdir("sys"):
        exit("No sys/ directory; please create before running this")

    try:
        outfile = open(OUTFILE, "w")
        os.chmod(OUTFILE, OUTFILE_MODE)
    except IOError as e:
        exit("Could not open {}: {}".format(OUTFILE, e))

    print("Creating {} using contents of sys/".format(OUTFILE))

    outfile.write(OUTFILE_HEADER.replace(os.path.basename(sys.argv[0]), OUTFILE))
    outfile.write(OUTFILE_FUNCS)
    outfile.write(OUTFILE_MAIN)

    process_sysdir(outfile)

    outfile.close()

    with tempfile.TemporaryDirectory() as tmpd:
        print("Recreating sys/ using {} at {}".format(OUTFILE, tmpd))
        os.system("./{script} {tmpd}".format(script=OUTFILE, tmpd=tmpd))
        verify_script(tmpd)

    print("Verification successful, {} is correct".format(OUTFILE))
