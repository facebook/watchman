# vim:ts=4:sw=4:et:
# Copyright 2012-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

# no unicode literals
from __future__ import absolute_import, division, print_function

import os

import pywatchman
import WatchmanTestCase


@WatchmanTestCase.expand_matrix
class TestSuffixGenerator(WatchmanTestCase.WatchmanTestCase):
    def test_suffix_generator(self):
        root = self.mkdtemp()

        self.touchRelative(root, "foo.c")
        os.mkdir(os.path.join(root, "subdir"))
        self.touchRelative(root, "subdir", "bar.txt")

        self.watchmanCommand("watch", root)

        self.assertFileListsEqual(
            self.watchmanCommand("query", root, {"suffix": "c", "fields": ["name"]})[
                "files"
            ],
            ["foo.c"],
        )

        self.assertFileListsEqual(
            self.watchmanCommand(
                "query", root, {"suffix": ["c", "txt"], "fields": ["name"]}
            )["files"],
            ["foo.c", "subdir/bar.txt"],
        )

        self.assertFileListsEqual(
            self.watchmanCommand(
                "query",
                root,
                {"suffix": ["c", "txt"], "relative_root": "subdir", "fields": ["name"]},
            )["files"],
            ["bar.txt"],
        )

        with self.assertRaises(pywatchman.WatchmanError) as ctx:
            self.watchmanCommand("query", root, {"suffix": {"a": "b"}})

        self.assertRegex(
            str(ctx.exception), "'suffix' must be a string or an array of strings"
        )

    def test_suffix_generator_empty(self):
        """Specifying no input suffixes should return no results.
        """
        root = self.mkdtemp()

        os.mkdir(os.path.join(root, "mydir"))
        os.mkdir(os.path.join(root, "mydir.dir"))
        self.touchRelative(root, "myfile")
        self.touchRelative(root, "myfile.txt")
        self.watchmanCommand("watch", root)

        self.assertFileListsEqual(
            self.watchmanCommand("query", root, {"fields": ["name"], "suffix": []})[
                "files"
            ],
            [],
        )
