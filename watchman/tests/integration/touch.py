# Copyright (c) Facebook, Inc. and its affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# Portable simple implementation of `touch`

# no unicode literals
from __future__ import absolute_import, division, print_function

import errno
import os
import sys


fname = sys.argv[1]

try:
    os.utime(fname, None)
except OSError as e:
    if e.errno == errno.ENOENT:
        with open(fname, "a"):
            os.utime(fname, None)
    else:
        raise
