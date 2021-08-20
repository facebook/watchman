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


args = sys.argv[1:]

if not args:
    args = ["-"]

for file_name in args:
    if file_name == "-":
        sys.stdout.write(sys.stdin.read())
    else:
        with open(file_name, "rb") as f:
            sys.stdout.write(f.read())
