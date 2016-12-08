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
class TestBSDish(WatchmanTestCase.WatchmanTestCase):
    def test_bsdish_toplevel(self):
        root = self.mkdtemp()
        os.mkdir(os.path.join(root, 'lower'))
        self.touchRelative(root, 'lower', 'file')
        self.touchRelative(root, 'top')

        self.watchmanCommand('watch', root)

        self.assertFileList(root, ['lower', 'lower/file', 'top'])

        find = self.watchmanCommand('find', root)
        clock = find['clock']

        since = self.watchmanCommand('since', root, clock)
        clock = since['clock']

        since = self.watchmanCommand('since', root, clock)
        self.assertFileListsEqual([], since['files'])
        clock = since['clock']

        os.unlink(os.path.join(root, 'top'))
        self.assertFileList(root, ['lower', 'lower/file'])

        now = self.watchmanCommand('since', root, clock)
        self.assertEqual(1, len(now['files']))
        self.assertFileListsEqual(['top'], [now['files'][0]['name']])
        self.assertFalse(now['files'][0]['exists'])
