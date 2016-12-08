# vim:ts=4:sw=4:et:
# Copyright 2012-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import WatchmanTestCase
import json
import tempfile
import os
import os.path


@WatchmanTestCase.expand_matrix
class TestFind(WatchmanTestCase.WatchmanTestCase):

    def test_find(self):
        root = self.mkdtemp()
        self.touchRelative(root, 'foo.c')
        self.touchRelative(root, 'bar.txt')

        self.watchmanCommand('watch', root)
        self.assertFileList(root, ['foo.c', 'bar.txt'])

        # Make sure we correctly observe deletions
        os.unlink(os.path.join(root, 'bar.txt'))
        self.assertFileList(root, ['foo.c'])

        # touch -> delete -> touch, should show up as exists
        self.touchRelative(root, 'bar.txt')
        self.assertFileList(root, ['foo.c', 'bar.txt'])
        os.unlink(os.path.join(root, 'bar.txt'))

        # A moderately more complex set of changes
        os.mkdir(os.path.join(root, 'adir'))
        os.mkdir(os.path.join(root, 'adir', 'subdir'))
        self.touchRelative(root, 'adir', 'subdir', 'file')
        os.rename(os.path.join(root, 'adir', 'subdir'),
                  os.path.join(root, 'adir', 'overhere'))

        self.assertFileList(root, [
          'adir',
          'adir/overhere',
          'adir/overhere/file',
          'foo.c'])

        os.rename(os.path.join(root, 'adir'),
                  os.path.join(root, 'bdir'))

        self.assertFileList(root, [
          'bdir',
          'bdir/overhere',
          'bdir/overhere/file',
          'foo.c'])

        self.assertTrue(self.rootIsWatched(root))

        self.watchmanCommand('watch-del', root)
        self.waitFor(lambda: not self.rootIsWatched(root))

        self.assertFalse(self.rootIsWatched(root))

