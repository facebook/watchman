#!/usr/bin/env python
# vim:ts=4:sw=4:et:

# This script is roughly equivalent to the configure script that is
# generated from configure.ac.

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import shlex

CSUFFIX = '.c'
OSUFFIX = '.o'
EXESUFFIX = '.exe'
CC = os.environ.get('CC', 'cc')
CPPFLAGS = os.environ.get('CPPFLAGS', '')
LDFLAGS = os.environ.get('LDFLAGS', '')

parser = argparse.ArgumentParser(description='Probe for system characteristics')
parser.add_argument('--cc', action='store', default=CC)
parser.add_argument('--cppflags', action='store', default=CPPFLAGS)
parser.add_argument('--ldflags', action='store', default=LDFLAGS)
parser.add_argument('--cwd', action='store', default=os.getcwd())
parser.add_argument('--configure', action='store', default='configure.ac')
parser.add_argument('--verbose', action='store_true')

args = parser.parse_args()

CC = args.cc
CPPFLAGS = shlex.split(args.cppflags)
LDFLAGS = shlex.split(args.ldflags)
os.chdir(args.cwd)

extra_flags = []
if sys.platform == 'darwin':
    extra_flags += ['-framework', 'CoreServices']

# List of functions that we may optionally use if they are present on
# the system.  Keep this sorted.
funcs_to_probe = [
    'FSEventStreamSetExclusionPaths',
    'accept4',
    'backtrace',
    'backtrace_symbols',
    'backtrace_symbols_fd',
    'fdopendir',
    'getattrlistbulk',
    'inotify_init',
    'inotify_init1',
    'kqueue',
    'localeconv',
    'memmem',
    'mkostemp',
    'openat',
    'pipe2',
    'port_create',
    'statfs',
    'strtoll',
    'sys_siglist',
]

# List of header files that we'd like to know about on the system.
# Keep this sorted.
headers_to_probe = [
    'CoreServices/CoreServices.h',
    'execinfo.h',
    'inttypes.h',
    'locale.h',
    'port.h',
    'sys/event.h',
    'sys/inotify.h',
    'sys/mount.h',
    'sys/param.h',
    'sys/resource.h',
    'sys/socket.h',
    'sys/statfs.h',
    'sys/statvfs.h',
    'sys/types.h',
    'sys/ucred.h',
    'sys/vfs.h',
    'valgrind/valgrind.h',
    'unistd.h',
]


def makesym(name):
    return re.sub('[./]', '_', name.upper())

def emit_status(proc, what, sym):
    out, err = proc.communicate()
    status = proc.wait()
    if args.verbose:
        print('// Status: %s' % status)
        for line in out.splitlines():
            print('// stdout: %s' % line)
        for line in err.splitlines():
            print('// stderr: %s' % line)
    if status == 0:
        print('#define %s 1' % sym)
    else:
        print('#undef %s' % sym)


def check_func(name):
    tmp_dir = tempfile.mkdtemp()
    try:
        src_file = os.path.join(tmp_dir, '%s%s' % (name, CSUFFIX))
        exe_file = os.path.join(tmp_dir, '%s%s' % (name, EXESUFFIX))

        with open(src_file, 'w+') as f:
            f.write(
                '''
int main(int argc, char**argv) {
    extern int %s(void);
    return %s();
}
''' % (name, name)
            )
        cmd = [CC] + CPPFLAGS + ['-o', exe_file, src_file] + \
              extra_flags + LDFLAGS
        what = '\n// Probing for function %s\n// %s' % (name, ' '.join(cmd))
        proc = subprocess.Popen(
            cmd, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE
        )
        emit_status(proc, what, 'HAVE_%s' % makesym(name))

    finally:
        shutil.rmtree(tmp_dir)


def check_header(name):
    tmp_dir = tempfile.mkdtemp()
    try:
        filename = os.path.basename(name)
        src_file = os.path.join(tmp_dir, '%s%s' % (filename, CSUFFIX))
        obj_file = os.path.join(tmp_dir, '%s%s' % (filename, OSUFFIX))

        with open(src_file, 'w+') as f:
            f.write('''
#include "%s"
''' % (name))

        cmd = [CC] + CPPFLAGS + [
            '-c',
            '-o',
            obj_file,
            src_file,
        ] + extra_flags
        what = '\n// Probing for function %s\n// %s' % (name, ' '.join(cmd))
        proc = subprocess.Popen(
            cmd, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE
        )
        emit_status(proc, what, 'HAVE_%s' % makesym(name))

    finally:
        shutil.rmtree(tmp_dir)


def check_struct_members(struct_name, member_name, includes=None):
    tmp_dir = tempfile.mkdtemp()
    try:
        filename = struct_name
        src_file = os.path.join(tmp_dir, '%s%s' % (filename, CSUFFIX))
        obj_file = os.path.join(tmp_dir, '%s%s' % (filename, OSUFFIX))

        with open(src_file, 'w+') as f:
            f.write(
                '''
%s
void *probe_%s_%s(struct %s *s) {
    return (void*)&s->%s;
}
''' % (includes, struct_name, member_name, struct_name, member_name)
            )
        cmd = [CC] + CPPFLAGS + ['-c', '-o', obj_file, src_file] + extra_flags
        what = '\n// Probing for %s::%s\n// %s' % (
            struct_name, member_name, ' '.join(cmd)
        )
        proc = subprocess.Popen(
            cmd, stdout=subprocess.PIPE,
            stderr=subprocess.PIPE
        )
        emit_status(
            proc, what,
            'HAVE_STRUCT_%s' % makesym('%s_%s' % (struct_name, member_name))
        )

    finally:
        shutil.rmtree(tmp_dir)


# Scrape out the version number from the configure script so that we have
# fewer places to update when we bump the version.
def extract_version():
    with open(args.configure, 'r') as f:
        for line in f:
            res = re.match('^AC_INIT\(\[watchman\], \[([0-9.]+)\]', line)
            if res:
                return res.group(1)
    raise Exception("couldn't find AC_INIT version line in " + args.configure)


print('// Generated by ./build/probe.py')
print('#ifndef WATCHMAN_CONFIG_H')
print('#define WATCHMAN_CONFIG_H')
print('#define PACKAGE_VERSION "%s"' % extract_version())
print('#define WATCHMAN_STATE_DIR "/var/facebook/watchman"')
print('#define WATCHMAN_CONFIG_FILE "/etc/watchman.json"')

for hname in headers_to_probe:
    check_header(hname)

for fname in funcs_to_probe:
    check_func(fname)

check_struct_members('statvfs', 'f_fstypename', '#include <sys/statvfs.h>')
check_struct_members('statvfs', 'f_basetype', '#include <sys/statvfs.h>')

print('#endif')
