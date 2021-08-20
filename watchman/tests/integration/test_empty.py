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
class TestEmpty(WatchmanTestCase.WatchmanTestCase):
    def test_empty(self):
        root = self.mkdtemp()
        self.touchRelative(root, "empty")
        with open(os.path.join(root, "notempty"), "w") as f:
            f.write("foo")

        self.watchmanCommand("watch", root)
        results = self.watchmanCommand(
            "query", root, {"expression": "empty", "fields": ["name"]}
        )

        self.assertEqual(["empty"], results["files"])

        results = self.watchmanCommand(
            "query", root, {"expression": "exists", "fields": ["name"]}
        )

        self.assertFileListsEqual(["empty", "notempty"], results["files"])

        clock = results["clock"]
        os.unlink(os.path.join(root, "empty"))

        self.assertFileList(root, files=["notempty"])

        results = self.watchmanCommand(
            "query", root, {"expression": "exists", "fields": ["name"]}
        )

        self.assertFileListsEqual(["notempty"], results["files"])

        # "files that don't exist" without a since term is absurd, so pass that in
        results = self.watchmanCommand(
            "query",
            root,
            {"since": clock, "expression": ["not", "exists"], "fields": ["name"]},
        )

        self.assertFileListsEqual(["empty"], results["files"])
