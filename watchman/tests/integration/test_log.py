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

import pywatchman
import WatchmanTestCase


@WatchmanTestCase.expand_matrix
class TestLog(WatchmanTestCase.WatchmanTestCase):
    def test_invalidNumArgsLogLevel(self):
        for params in [["log-level"], ["log-level", "debug", "extra"]]:
            with self.assertRaises(pywatchman.WatchmanError) as ctx:
                self.watchmanCommand(*params)

            self.assertIn("wrong number of arguments", str(ctx.exception))

    def test_invalidLevelLogLevel(self):
        with self.assertRaises(pywatchman.WatchmanError) as ctx:
            self.watchmanCommand("log-level", "invalid")

        self.assertIn("invalid log level", str(ctx.exception))

    def test_invalidNumArgsLog(self):
        for params in [["log"], ["log", "debug"], ["log", "debug", "test", "extra"]]:
            with self.assertRaises(pywatchman.WatchmanError) as ctx:
                self.watchmanCommand(*params)

            self.assertIn("wrong number of arguments", str(ctx.exception))

    def test_invalidLevelLog(self):
        with self.assertRaises(pywatchman.WatchmanError) as ctx:
            self.watchmanCommand("log", "invalid", "test")

        self.assertIn("invalid log level", str(ctx.exception))
