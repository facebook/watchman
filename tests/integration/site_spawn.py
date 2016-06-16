#!/usr/bin/env python
# This is a simple script that spawns a watchman process in the background
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals
import os
import sys
import subprocess
args = sys.argv[1:]
args.insert(0, 'watchman')
args.insert(1, '--foreground')
subprocess.Popen(args)
