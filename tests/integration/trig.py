#!/usr/bin/env python
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import sys
import time
log_file_name = sys.argv[1]

args = sys.argv[2:]

with open(log_file_name, 'a') as f:
    for arg in args:
        f.write('%s ' % time.time())
        f.write(arg)
        f.write('\n')

print('WOOT from trig.sh')
