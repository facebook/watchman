#!/usr/bin/env python
import sys
import time
log_file_name = sys.argv[1]

# Copy json from stdin to the log file
with open(log_file_name, 'a') as f:
    print('trigjson.py: Copying STDIN to %s' % log_file_name)
    json_in = sys.stdin.read()
    print('stdin: %s' % json_in)
    f.write(json_in)

