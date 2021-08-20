# vim:ts=4:sw=4:et:
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

import os

import pywatchman
import WatchmanTestCase


@WatchmanTestCase.expand_matrix
class TestBig(WatchmanTestCase.WatchmanTestCase):
    def checkOSApplicability(self):
        if os.name == "nt":
            self.skipTest("Windows has problems with this test")

    def test_bigQuery(self):
        root = self.mkdtemp()

        self.watchmanCommand("watch", root)

        # Create a huge query.  We're shooting for more than 2MB;
        # the server buffer size is 1MB and we want to make sure
        # we need more than 2 chunks, and we want to tickle some
        # buffer boundary conditions

        base = 2 * 1024 * 1024

        for size in range(base - 256, base + 2048, 63):
            try:
                res = self.watchmanCommand(
                    "query", root, {"expression": ["name", "a" * size]}
                )

                self.assertEqual([], res["files"])
            except pywatchman.WatchmanError as e:
                # We don't want to print the real command, as
                # it is too long, instead, replace it with
                # a summary of the size that we picked
                e.cmd = "big query with size %d" % size

                if self.transport == "cli":
                    e.cmd = "%s\n%s" % (e.cmd, self.getLogSample())
                raise
