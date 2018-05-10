#!/usr/bin/env python
# no unicode literals
from __future__ import absolute_import, division, print_function

import sys


args = sys.argv[1:]

if not args:
    args = ["-"]

for file_name in args:
    if file_name == "-":
        sys.stdout.write(sys.stdin.read())
    else:
        with open(file_name, "rb") as f:
            sys.stdout.write(f.read())
