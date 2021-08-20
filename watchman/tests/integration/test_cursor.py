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
class TestCursor(WatchmanTestCase.WatchmanTestCase):
    def test_cursor(self):
        root = self.mkdtemp()
        self.watchmanCommand("watch", root)
        self.assertFileList(root, files=[], cursor="n:testCursor")

        # touch a file; it should show as `new` when we next use the cursor
        self.touchRelative(root, "one")
        result = self.watchmanCommand(
            "query", root, {"since": "n:testCursor", "fields": ["name", "new"]}
        )

        self.assertEqual(result["files"], [{"name": "one", "new": True}])

        # nothing changed, so we expect no changes in this run
        self.assertFileList(root, files=[], cursor="n:testCursor")

        # Now touch the same file again; it should not show as new
        # when we run the next query
        self.touchRelative(root, "one")
        result = self.watchmanCommand(
            "query", root, {"since": "n:testCursor", "fields": ["name", "new"]}
        )

        self.assertEqual(result["files"], [{"name": "one", "new": False}])

        # Deleted files shouldn't show up in fresh cursors
        self.touchRelative(root, "two")
        os.unlink(os.path.join(root, "one"))
        result = self.watchmanCommand(
            "query",
            root,
            {"since": "n:testCursor2", "fields": ["name", "new", "exists"]},
        )
        self.assertTrue(result["is_fresh_instance"])
        self.assertEqual(
            result["files"], [{"name": "two", "new": True, "exists": True}]
        )

        # ... but they should show up afterwards
        os.unlink(os.path.join(root, "two"))

        result = self.watchmanCommand(
            "query",
            root,
            {"since": "n:testCursor2", "fields": ["name", "new", "exists"]},
        )
        self.assertEqual(
            result["files"], [{"name": "two", "new": False, "exists": False}]
        )
