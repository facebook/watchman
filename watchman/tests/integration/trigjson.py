#!/usr/bin/env python
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

# no unicode literals
from __future__ import absolute_import, division, print_function

import sys
import time


log_file_name = sys.argv[1]

# Copy json from stdin to the log file
with open(log_file_name, "a") as f:
    print("trigjson.py: Copying STDIN to %s" % log_file_name)
    json_in = sys.stdin.read()
    print("stdin: %s" % json_in)
    f.write(json_in)
