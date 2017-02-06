#!/usr/bin/python3

OUTFILE_HEADER = u"""#!/usr/bin/python3
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


OUTFILE = u"sys-script.py"
OUTFILE_MODE = stat.S_IRWXU | stat.S_IRWXG | stat.S_IROTH | stat.S_IXOTH

OUTFILE_FUNCS = r"""
import os, sys

def d(path, mode):
    os.mkdir(path)
    os.chmod(path, int(mode, 8))

def l(path, src):
    os.symlink(src, path)

def f(path, mode, contents):
    # wrap string contents with b''''''
    s = u"b'''{}'''".format(contents)
    # convert directly to bytes using eval
    b = eval(s)
    # write the bytes out to file
    outf = open(path, "wb")
    outf.write(b)
    outf.close()
    os.chmod(path, int(mode, 8))

"""

OUTFILE_MAIN = u"""
if len(sys.argv) < 2:
    print(u"Usage: {} <target dir>".format(sys.argv[0]))
    exit(1)

if not os.path.isdir(sys.argv[1]):
    print(u"Target dir {} not found".format(sys.argv[1]))
    exit(1)

os.chdir(sys.argv[1])

"""


def mode_rwx(m):
    return m & ( stat.S_IRWXU | stat.S_IRWXG | stat.S_IRWXO )


def handle_dir(outfile, path):
    m = mode_rwx(os.lstat(path).st_mode)
    outfile.write(u"d(u'{}', u'{:o}')\n".format(path, m))


def handle_link(outfile, path):
    src = os.readlink(path)
    outfile.write(u"l(u'{}', u'{}')\n".format(path, src))


def handle_file(outfile, path):
    m = mode_rwx(os.lstat(path).st_mode)
    f = open(path, "rb")
    # read in as binary
    b = f.read()
    f.close()
    # convert to string containing b'' representation
    s = repr(b)
    # strip leading b' and trailing '
    s = s[2:-1]
    # b'' contain newlines as \n, so convert back
    s = s.replace(u'\\n', u'\n')
    # we're placing it into a r'''''' so replace '
    s = s.replace(u"'", u'\x27')
    outfile.write(u"f(u'{}', u'{:o}', r'''{}''')\n".format(path, m, s))


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
        raise Exception(u"Not directory")
    if mode_rwx(mode_a) != mode_rwx(mode_b):
        raise Exception(u"Permissions mismatch")


def verify_link(tmpd, path_a):
    path_b = os.path.join(tmpd, path_a)
    if not stat.S_ISLNK(os.lstat(path_b).st_mode):
        raise Exception(u"Not symlink")
    if os.readlink(path_a) != os.readlink(path_b):
        raise Exception(u"Symlink dest mismatch")


def verify_file(tmpd, path_a):
    path_b = os.path.join(tmpd, path_a)
    mode_a = os.lstat(path_a).st_mode
    mode_b = os.lstat(path_b).st_mode
    if not stat.S_ISREG(mode_b):
        raise Exception(u"Not file")
    if mode_rwx(mode_a) != mode_rwx(mode_b):
        raise Exception(u"Permissions mismatch")
    if not filecmp.cmp(path_a, path_b, shallow=False):
        raise Exception(u"File contents mismatch")


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
        print(e)
        exit(1)


if __name__ == "__main__":
    # Always operate in the dir where this script is
    os.chdir(os.path.dirname(sys.argv[0]))

    if not os.path.isdir("sys"):
        print("No sys/ directory; please create before running this")
        exit(1)

    try:
        outfile = open(OUTFILE, "w")
        os.chmod(OUTFILE, OUTFILE_MODE)
    except IOError as e:
        printf("Could not open %s: %s" % (OUTFILE, e))
        exit(1)

    print("Creating %s using contents of sys/" % OUTFILE)

    outfile.write(OUTFILE_HEADER.replace(os.path.basename(sys.argv[0]), OUTFILE))
    outfile.write(OUTFILE_FUNCS)
    outfile.write(OUTFILE_MAIN)

    process_sysdir(outfile)

    outfile.close()

    with tempfile.TemporaryDirectory() as tmpd:
        print("Recreating sys/ using {} at {}".format(OUTFILE, tmpd))
        os.system(u"./{script} {tmpd}".format(script=OUTFILE, tmpd=tmpd))
        verify_script(tmpd)

    print("Verification successful, {} is correct".format(OUTFILE))
