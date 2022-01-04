#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# This is a simple script that fails to spawn a process in the background
# no unicode literals
from __future__ import absolute_import, division, print_function

import sys


print("failed to start")
sys.stdout.flush()
sys.exit(1)
