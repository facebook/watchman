# Portable simple implementation of `touch`

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

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

