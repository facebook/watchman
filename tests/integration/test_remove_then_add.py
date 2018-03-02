# vim:ts=4:sw=4:et:
# Copyright 2012-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import WatchmanTestCase
import os
import shutil


@WatchmanTestCase.expand_matrix
class TestRemoveThenAdd(WatchmanTestCase.WatchmanTestCase):
    def checkOSApplicability(self):
        if os.name == 'linux' and os.getenv('TRAVIS'):
            self.skipTest('openvz and inotify unlinks == bad time')

    def test_remove_then_add(self):
        root = self.mkdtemp()
        os.mkdir(os.path.join(root, 'foo'))

        self.watchmanCommand('watch', root)

        self.touchRelative(root, 'foo', '222')
        os.mkdir(os.path.join(root, 'foo', 'bar'))

        self.assertFileList(root, files=['foo', 'foo/bar', 'foo/222'])

        shutil.rmtree(os.path.join(root, 'foo', 'bar'))
        self.removeRelative(root, 'foo', '222')
        shutil.rmtree(os.path.join(root, 'foo'))

        self.assertFileList(root, files=[])

        os.mkdir(os.path.join(root, 'foo'))
        self.touchRelative(root, 'foo', '222')

        self.assertFileList(root, files=['foo', 'foo/222'])
