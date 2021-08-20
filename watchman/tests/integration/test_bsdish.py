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
class TestBSDish(WatchmanTestCase.WatchmanTestCase):
    def test_bsdish_toplevel(self):
        root = self.mkdtemp()
        os.mkdir(os.path.join(root, "lower"))
        self.touchRelative(root, "lower", "file")
        self.touchRelative(root, "top")

        watch = self.watchmanCommand("watch", root)

        self.assertFileList(root, ["lower", "lower/file", "top"])

        find = self.watchmanCommand("find", root)
        clock = find["clock"]

        since = self.watchmanCommand("since", root, clock)
        clock = since["clock"]

        since = self.watchmanCommand(
            "query", root, {"expression": ["allof", ["since", clock], ["type", "f"]]}
        )
        self.assertFileListsEqual([], since["files"])
        clock = since["clock"]

        os.unlink(os.path.join(root, "top"))
        self.assertFileList(root, ["lower", "lower/file"])

        now = self.watchmanCommand("since", root, clock)
        expected = ["top"]
        if watch["watcher"] == "kqueue+fsevents":
            # For the split watch, a cookie is being written to each top level
            # directory, and thus the "lower" directory will be reported as
            # having been changed.
            expected.append("lower")
        self.assertEqual(len(expected), len(now["files"]))
        self.assertFileListsEqual(
            expected, list(map(lambda x: x["name"], now["files"]))
        )
        for f in now["files"]:
            if f["name"] == "top":
                self.assertFalse(f["exists"])
