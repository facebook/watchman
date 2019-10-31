# vim:ts=4:sw=4:et:
# Copyright 2012-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

# no unicode literals
from __future__ import absolute_import, division, print_function

import os

import pywatchman
import WatchmanTestCase


@WatchmanTestCase.expand_matrix
class TestTypeExpr(WatchmanTestCase.WatchmanTestCase):
    def test_type_expr(self):
        root = self.mkdtemp()

        self.touchRelative(root, "foo.c")
        os.mkdir(os.path.join(root, "subdir"))
        self.touchRelative(root, "subdir", "bar.txt")

        self.watchmanCommand("watch", root)

        self.assertFileListsEqual(
            self.watchmanCommand(
                "query", root, {"expression": ["type", "f"], "fields": ["name"]}
            )["files"],
            ["foo.c", "subdir/bar.txt"],
        )

        self.assertEqual(
            self.watchmanCommand(
                "query", root, {"expression": ["type", "d"], "fields": ["name", "type"]}
            )["files"],
            [{"name": "subdir", "type": "d"}],
        )

        with self.assertRaises(pywatchman.WatchmanError) as ctx:
            self.watchmanCommand("query", root, {"expression": ["type", "x"]})

        self.assertIn("invalid type string 'x'", str(ctx.exception))

        with self.assertRaises(pywatchman.WatchmanError) as ctx:
            self.watchmanCommand("query", root, {"expression": "type"})

        self.assertIn(
            '"type" term requires a type string parameter', str(ctx.exception)
        )

        with self.assertRaises(pywatchman.WatchmanError) as ctx:
            self.watchmanCommand("query", root, {"expression": ["type", 123]})

        self.assertIn(
            'First parameter to "type" term must be a type string', str(ctx.exception)
        )
