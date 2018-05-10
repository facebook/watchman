# vim:ts=4:sw=4:et:
# Copyright 2012-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

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
            ("failed to parse query: sync_timeout " "must be an integer value >= 0"),
            str(ctx.exception),
        )

        res = self.watchmanCommand(
            "query", root, {"expression": ["exists"], "sync_timeout": 200}
        )
        self.assertEqual(res["files"], [])
