# vim:ts=4:sw=4:et:
# Copyright 2012-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

# no unicode literals
from __future__ import absolute_import, division, print_function

import os
import time

import WatchmanTestCase


@WatchmanTestCase.expand_matrix
class TestSinceTerm(WatchmanTestCase.WatchmanTestCase):

    def test_since_term(self):
        root = self.mkdtemp()

        self.touchRelative(root, "foo.c")
        os.mkdir(os.path.join(root, "subdir"))
        self.touchRelative(root, "subdir", "bar.txt")

        self.watchmanCommand("watch", root)

        res = self.watchmanCommand("find", root, "foo.c")
        first_clock = res["clock"]
        base_mtime = res["files"][0]["mtime"]

        # Since is GT not GTE
        res = self.watchmanCommand(
            "query",
            root,
            {"expression": ["since", base_mtime, "mtime"], "fields": ["name"]},
        )
        self.assertFileListsEqual(res["files"], ["foo.c", "subdir", "subdir/bar.txt"])

        res = self.watchmanCommand(
            "query",
            root,
            {
                "expression": [
                    "allof",
                    ["since", base_mtime - 1, "mtime"],
                    ["name", "foo.c"],
                ],
                "fields": ["name"],
            },
        )
        self.assertFileListsEqual(res["files"], ["foo.c"])

        if self.isCaseInsensitive():
            res = self.watchmanCommand(
                "query",
                root,
                {
                    "expression": [
                        "allof",
                        ["since", base_mtime - 1, "mtime"],
                        ["name", "FOO.c"],
                    ],
                    "fields": ["name"],
                },
            )
            self.assertFileListsEqual(res["files"], ["foo.c"])

        # Try with a clock
        res = self.watchmanCommand(
            "query", root, {"expression": ["since", first_clock], "fields": ["name"]}
        )
        self.assertFileListsEqual(res["files"], [])

        future = base_mtime + 15
        self.touch(os.path.join(root, "foo.c"), (future, future))

        # Try again with a clock
        res = self.watchmanCommand(
            "query", root, {"expression": ["since", first_clock], "fields": ["name"]}
        )
        self.assertFileListsEqual(res["files"], ["foo.c"])

        # And check that we're still later than a later but not current mtime
        res = self.watchmanCommand(
            "query",
            root,
            {"expression": ["since", base_mtime + 5, "mtime"], "fields": ["name"]},
        )
        self.assertFileListsEqual(res["files"], ["foo.c"])

        # If using a timestamp against the oclock, ensure that we're comparing
        # in the correct order.  We need to force a 2 second delay so that the
        # timestamp moves forward by at least 1 increment for this test to
        # work correctly
        time.sleep(2)

        res = self.watchmanCommand(
            "query",
            root,
            {
                "expression": ["allof", ["since", int(time.time())], ["name", "foo.c"]],
                "fields": ["name"],
            },
        )
        self.assertFileListsEqual(res["files"], [])

        # Try with a fresh clock instance; we must only return files that exist.
        self.removeRelative(root, "subdir", "bar.txt")
        self.assertFileList(root, files=["foo.c", "subdir"], cursor="c:0:0")
