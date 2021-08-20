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

import WatchmanTestCase


@WatchmanTestCase.expand_matrix
class TestClock(WatchmanTestCase.WatchmanTestCase):
    def test_clock(self):
        root = self.mkdtemp()
        self.watchmanCommand("watch", root)
        clock = self.watchmanCommand("clock", root)

        self.assertRegex(clock["clock"], "^c:\\d+:\\d+:\\d+:\\d+$")

    def test_clock_sync(self):
        root = self.mkdtemp()
        self.watchmanCommand("watch", root)
        clock1 = self.watchmanCommand("clock", root, {"sync_timeout": 5000})
        self.assertRegex(clock1["clock"], "^c:\\d+:\\d+:\\d+:\\d+$")

        clock2 = self.watchmanCommand("clock", root, {"sync_timeout": 5000})
        self.assertRegex(clock2["clock"], "^c:\\d+:\\d+:\\d+:\\d+$")

        self.assertNotEqual(clock1, clock2)
