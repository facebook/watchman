#!/usr/bin/env python
from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import os

os.environ['PWD'] = os.getcwd()

for k, v in os.environ.items():
    print('%s=%s' % (k, v))
