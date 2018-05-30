# vim:ts=4:sw=4:et:
# Copyright 2012-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

# no unicode literals
from __future__ import absolute_import, division, print_function

import os

import WatchmanTestCase


@WatchmanTestCase.expand_matrix
class TestWatchDelAll(WatchmanTestCase.WatchmanTestCase):
    def test_watch_del_all(self):
        root = self.mkdtemp()

        dirs = [os.path.join(root, f) for f in ["a", "b", "c", "d"]]

        for d in dirs:
            os.mkdir(d)
            self.touchRelative(d, "foo")
            self.watchmanCommand("watch", d)
            self.assertFileList(d, files=["foo"])

        self.watchmanCommand("watch-del-all")
        self.assertEqual(self.watchmanCommand("watch-list")["roots"], [])
