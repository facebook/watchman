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
class TestSizeExpr(WatchmanTestCase.WatchmanTestCase):
    def test_size_expr(self):
        root = self.mkdtemp()

        self.touchRelative(root, "empty")
        with open(os.path.join(root, "notempty"), "w") as f:
            f.write("foo")

        with open(os.path.join(root, "1k"), "w") as f:
            f.truncate(1024)

        self.watchmanCommand("watch", root)

        tests = [
            ["eq", 0, ["empty"]],
            ["ne", 0, ["1k", "notempty"]],
            ["gt", 0, ["1k", "notempty"]],
            ["gt", 2, ["1k", "notempty"]],
            ["ge", 3, ["1k", "notempty"]],
            ["gt", 3, ["1k"]],
            ["le", 3, ["empty", "notempty"]],
            ["lt", 3, ["empty"]],
        ]

        for (op, operand, expect) in tests:
            res = self.watchmanCommand(
                "query", root, {"expression": ["size", op, operand], "fields": ["name"]}
            )
            self.assertFileListsEqual(
                res["files"], expect, message=repr((op, operand, expect))
            )

        self.removeRelative(root, "1k")
        self.assertFileListsEqual(
            self.watchmanCommand(
                "query", root, {"expression": ["size", "gt", 100], "fields": ["name"]}
            )["files"],
            [],
            message="removed file is not matched",
        )
