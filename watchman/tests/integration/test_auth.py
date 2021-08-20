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
class TestAuth(WatchmanTestCase.WatchmanTestCase):
    def requiresPersistentSession(self):
        return True

    def test_dropPriv(self):
        root = self.mkdtemp()
        self.touchRelative(root, "111")

        self.watchmanCommand("watch", root)

        # pretend we are not the owner
        self.watchmanCommand("debug-drop-privs")

        # Should be able to watch something that is already watched
        self.watchmanCommand("watch", root)

        # can't make a new watch
        altroot = self.mkdtemp()
        with self.assertRaises(pywatchman.WatchmanError):
            self.watchmanCommand("watch", altroot)

        # Should not be able to delete a watch
        with self.assertRaises(pywatchman.WatchmanError):
            self.watchmanCommand("watch-del", root)

        # or register a trigger
        with self.assertRaises(pywatchman.WatchmanError):
            self.watchmanCommand("trigger", root, "trig", "*.js", "--", "false")
