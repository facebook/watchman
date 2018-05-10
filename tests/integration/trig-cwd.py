#!/usr/bin/env python
# no unicode literals
from __future__ import absolute_import, division, print_function

import os


os.environ["PWD"] = os.getcwd()

for k, v in os.environ.items():
    print("%s=%s" % (k, v))
