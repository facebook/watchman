#!/usr/bin/env python3
# This is a simple script that fails to spawn a process in the background
# no unicode literals
from __future__ import absolute_import, division, print_function

import sys


print("failed to start")
sys.stdout.flush()
sys.exit(1)
