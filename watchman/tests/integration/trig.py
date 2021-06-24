#!/usr/bin/env python
# no unicode literals
from __future__ import absolute_import, division, print_function

import sys
import time


log_file_name = sys.argv[1]

args = sys.argv[2:]

with open(log_file_name, "a") as f:
    for arg in args:
        f.write("%s " % time.time())
        f.write(arg)
        f.write("\n")

print("WOOT from trig.sh")
