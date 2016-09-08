# vim:ts=4:sw=4:et:
# Copyright 2016-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import WatchmanTestCase
import tempfile
import os
import os.path
import time
import shutil
import time


@WatchmanTestCase.expand_matrix
class TestDirMove(WatchmanTestCase.WatchmanTestCase):

    # testing this is flaky at best on windows due to latency
    # and exclusivity of file handles, so skip it.
    def checkOSApplicability(self):
        if os.name == 'nt':
            self.skipTest('windows is too flaky for this test')

    def build_under(self, root, name, latency=0):
        os.mkdir(os.path.join(root, name))
        if latency > 0:
            time.sleep(latency)
        self.touch(os.path.join(root, name, 'a'))

    def test_atomicMove(self):
        root = self.mkdtemp()

        dir_of_interest = os.path.join(root, 'dir')
        alt_dir = os.path.join(root, 'alt')
        dead_dir = os.path.join(root, 'bye')

        self.build_under(root, 'dir')
        self.watchmanCommand('watch', root)
        self.assertFileList(root, ['dir', 'dir/a'])

        # build out a replacement dir
        self.build_under(root, 'alt')

        os.rename(dir_of_interest, dead_dir)
        os.rename(alt_dir, dir_of_interest)

        self.assertFileList(root, ['dir', 'dir/a', 'bye', 'bye/a'])

    def test_NonAtomicMove(self):
        root = self.mkdtemp()

        dir_of_interest = os.path.join(root, 'dir')
        alt_dir = os.path.join(root, 'alt')

        self.build_under(root, 'dir')
        self.watchmanCommand('watch', root)
        self.assertFileList(root, ['dir', 'dir/a'])

        shutil.rmtree(dir_of_interest)
        self.build_under(root, 'dir', latency=1)

        self.assertFileList(root, ['dir', 'dir/a'])
