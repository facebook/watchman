# Portable simple implementation of `touch`
import os
import sys
import errno

fname = sys.argv[1]

try:
    os.utime(fname, None)
except OSError as e:
    if e.errno == errno.ENOENT:
        with open(fname, 'a'):
            os.utime(fname, None)
    else:
        raise

