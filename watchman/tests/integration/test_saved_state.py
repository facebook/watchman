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
import WatchmanSCMTestCase
import WatchmanTestCase


@WatchmanTestCase.expand_matrix
class TestSavedState(WatchmanSCMTestCase.WatchmanSCMTestCase):
    def setUp(self):
        self.skipIfNoFSMonitor()
        self.root = self.mkdtemp()
        # This test does not require much so just create a super simple repo
        self.hg(["init"], cwd=self.root)
        self.touchRelative(self.root, "foo")
        self.hg(["book", "initial"], cwd=self.root)
        self.hg(["addremove"], cwd=self.root)
        self.hg(["commit", "-m", "initial"], cwd=self.root)
        self.touchRelative(self.root, "bar")
        self.touchRelative(self.root, "car")
        self.hg(["addremove"], cwd=self.root)
        self.hg(["commit", "-m", "add bar and car"], cwd=self.root)
        self.hg(["book", "TheMaster"], cwd=self.root)
        self.watchmanCommand("watch", self.root)

    def get_skeleton_query(self):
        return {
            "expression": [
                "not",
                ["anyof", ["name", ".hg"], ["match", "hg-check*"], ["dirname", ".hg"]],
            ],
            "fields": ["name"],
            "since": {"scm": {"mergebase-with": "TheMaster"}},
        }

    def test_unsupportedStorageType(self):
        # If the storage type is not supported, watchman should throw
        test_query = self.get_skeleton_query()
        test_query["since"]["scm"]["saved-state"] = {"storage": "foo", "config": {}}
        with self.assertRaises(pywatchman.WatchmanError) as ctx:
            self.watchmanCommand("query", self.root, test_query)
        self.assertIn("invalid storage type 'foo'", str(ctx.exception))
