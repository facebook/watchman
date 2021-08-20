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
class TestInvalidExpr(WatchmanTestCase.WatchmanTestCase):
    def test_invalid_expr_term(self):
        root = self.mkdtemp()
        self.watchmanCommand("watch", root)

        with self.assertRaises(pywatchman.WatchmanError) as ctx:
            self.watchmanCommand(
                "query",
                root,
                {
                    "expression": [
                        "allof",
                        "dont-implement-this-term",
                        ["anyof", ["suffix", "apcarc"]],
                    ]
                },
            )

        self.assertIn(
            (
                "failed to parse query: unknown expression "
                "term 'dont-implement-this-term'"
            ),
            str(ctx.exception),
        )

    def test_invalid_sync_timeout(self):
        root = self.mkdtemp()
        self.watchmanCommand("watch", root)

        with self.assertRaises(pywatchman.WatchmanError) as ctx:
            self.watchmanCommand(
                "query", root, {"expression": ["exists"], "sync_timeout": -1}
            )

        self.assertIn(
            "failed to parse query: sync_timeout must be an integer value >= 0",
            str(ctx.exception),
        )

        res = self.watchmanCommand(
            "query", root, {"expression": ["exists"], "sync_timeout": 200}
        )
        self.assertEqual(res["files"], [])
