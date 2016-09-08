#!/usr/bin/env python
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import sys

args = sys.argv[1:]

if not args:
    args = ['-']

for file_name in args:
    if file_name == '-':
        sys.stdout.write(sys.stdin.read())
    else:
        with open(file_name, 'rb') as f:
            sys.stdout.write(f.read())
