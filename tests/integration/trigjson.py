#!/usr/bin/env python
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals
import sys
import time
log_file_name = sys.argv[1]

# Copy json from stdin to the log file
with open(log_file_name, 'a') as f:
    print('trigjson.py: Copying STDIN to %s' % log_file_name)
    json_in = sys.stdin.read()
    print('stdin: %s' % json_in)
    f.write(json_in)

