# Copyright 2017-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

# no unicode literals
from __future__ import absolute_import, division, print_function

import os

import WatchmanTestCase


@WatchmanTestCase.expand_matrix
class TestCaseSensitive(WatchmanTestCase.WatchmanTestCase):

    def test_changeCase(self):
        root = self.mkdtemp()
        os.mkdir(os.path.join(root, "foo"))
        self.watchmanCommand("watch", root)
        self.assertFileList(root, ["foo"])

        os.rename(os.path.join(root, "foo"), os.path.join(root, "FOO"))
        self.touchRelative(root, "FOO", "bar")
        self.assertFileList(root, ["FOO", "FOO/bar"])

        os.rename(os.path.join(root, "FOO", "bar"), os.path.join(root, "FOO", "BAR"))
        self.assertFileList(root, ["FOO", "FOO/BAR"])

        os.rename(os.path.join(root, "FOO"), os.path.join(root, "foo"))
        self.assertFileList(root, ["foo", "foo/BAR"])

        os.mkdir(os.path.join(root, "foo", "baz"))
        self.touchRelative(root, "foo", "baz", "file")
        self.assertFileList(root, ["foo", "foo/BAR", "foo/baz", "foo/baz/file"])

        os.rename(os.path.join(root, "foo"), os.path.join(root, "Foo"))

        self.assertFileList(root, ["Foo", "Foo/BAR", "Foo/baz", "Foo/baz/file"])
