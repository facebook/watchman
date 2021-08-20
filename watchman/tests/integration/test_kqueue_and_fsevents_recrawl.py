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
import os.path
import sys

import WatchmanTestCase


@WatchmanTestCase.expand_matrix
class TestKQueueAndFSEventsRecrawl(WatchmanTestCase.WatchmanTestCase):
    def checkOSApplicability(self):
        if sys.platform != "darwin":
            self.skipTest("N/A unless macOS")

    def test_recrawl(self):
        root = self.mkdtemp()
        watch = self.watchmanCommand("watch", root)

        # On macOS, we may not always use kqueue+fsevents
        if watch["watcher"] != "kqueue+fsevents":
            return

        os.mkdir(os.path.join(root, "foo"))
        filelist = ["foo"]

        self.assertFileList(root, filelist)

        self.suspendWatchman()

        filelist = ["foo"]
        for i in range(3000):
            self.touchRelative(root, "foo", str(i))
            filelist.append(f"foo/{i}")

        self.resumeWatchman()

        self.watchmanCommand(
            "debug-kqueue-and-fsevents-recrawl", root, os.path.join(root, "foo")
        )

        self.assertFileList(root, filelist)
