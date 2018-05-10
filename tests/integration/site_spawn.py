#!/usr/bin/env python
# This is a simple script that spawns a watchman process in the background
# no unicode literals
from __future__ import absolute_import, division, print_function

import os
import subprocess
import sys


args = sys.argv[1:]
args.insert(0, "watchman")
args.insert(1, "--foreground")
subprocess.Popen(args)
