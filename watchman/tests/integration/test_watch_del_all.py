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

import WatchmanTestCase


@WatchmanTestCase.expand_matrix
class TestWatchDelAll(WatchmanTestCase.WatchmanTestCase):
    def test_watch_del_all(self):
        root = self.mkdtemp()

        dirs = [os.path.join(root, f) for f in ["a", "b", "c", "d"]]

        for d in dirs:
            os.mkdir(d)
            self.touchRelative(d, "foo")
            self.watchmanCommand("watch", d)
            self.assertFileList(d, files=["foo"])

        self.watchmanCommand("watch-del-all")
        self.assertEqual(self.watchmanCommand("watch-list")["roots"], [])
