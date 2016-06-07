#!/usr/bin/env python
import sys
import time
log_file_name = sys.argv[1]

args = sys.argv[2:]

with open(log_file_name, 'a') as f:
    for arg in args:
        f.write('%s %s\n' % (time.time(), arg))

print 'WOOT from trig.sh'
