# vim:ts=4:sw=4:et:
# Copyright 2012-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import WatchmanTestCase
import os


@WatchmanTestCase.expand_matrix
class TestWatchDelAll(WatchmanTestCase.WatchmanTestCase):
    def test_watch_del_all(self):
        root = self.mkdtemp()

        dirs = [os.path.join(root, f) for f in ['a', 'b', 'c', 'd']]

        for d in dirs:
            os.mkdir(d)
            self.touchRelative(d, 'foo')
            self.watchmanCommand('watch', d)
            self.assertFileList(d, files=['foo'])

        self.watchmanCommand('watch-del-all')
        self.assertEqual(
            self.watchmanCommand('watch-list')['roots'],
            [])
