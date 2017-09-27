# Copyright 2015-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import WatchmanTestCase
import os
import os.path
import WatchmanWaitInstance
import WatchmanInstance


@WatchmanTestCase.expand_matrix
class TestWatchmanWait(WatchmanTestCase.WatchmanTestCase):
    def requiresPersistentSession(self):
        return True

    def assertSyncWatchmanWait(self, wi, root, maxTries=4):
        cookie = '.watchmanWaitCookie'

        for _ in range(maxTries):
            self.touchRelative(root, cookie)
            res = wi.readLine(timeout=2)
            if res and cookie in res:
                break
        if not res:
            raise Exception("Failed to communicate with watchman-watch")

        # Read any remaining notifications (clear the buffer for tests)
        wi.readLines(count=5, timeout=2)

    def assertWaitedFileList(self, wi, expected, msg=None):
        res = wi.readLines(count=len(expected))
        self.assertFileListsEqual(res, expected, message=msg)

    def test_wait(self):

        root = self.mkdtemp()
        self.touchRelative(root, 'foo')
        a_dir = os.path.join(root, 'a')
        os.mkdir(a_dir)
        self.touchRelative(a_dir, 'foo')

        wi = WatchmanWaitInstance.Instance(
            sock_path=WatchmanInstance.getSharedInstance().getSockPath()
        )
        wi.start(paths=[root], cmdArgs=['--relative', root])
        self.assertSyncWatchmanWait(wi, root)

        self.touchRelative(root, 'bar')
        self.removeRelative(root, 'foo')
        self.assertWaitedFileList(wi, ['foo', 'bar'], msg="wait, create files")

        self.touchRelative(a_dir, 'bar')
        self.removeRelative(a_dir, 'foo')
        self.assertWaitedFileList(
            wi, ['a/bar', 'a/foo', 'a'], msg="wait, create dir")

        b_dir = os.path.join(root, 'b')
        os.mkdir(b_dir)
        self.touchRelative(b_dir, 'foo')
        self.assertWaitedFileList(wi, ['b', 'b/foo'], msg=None)

    def test_rel_root(self):
        root = self.mkdtemp()

        a_dir = os.path.join(root, 'a')
        os.mkdir(a_dir)
        b_dir = os.path.join(root, 'b')
        os.mkdir(b_dir)

        wi = WatchmanWaitInstance.Instance(
            sock_path=WatchmanInstance.getSharedInstance().getSockPath()
        )
        wi.start(paths=[a_dir, b_dir], cmdArgs=['--relative', b_dir])

        self.assertSyncWatchmanWait(wi, a_dir)
        self.touchRelative(a_dir, 'afoo')
        self.touchRelative(b_dir, 'bfoo')
        self.assertWaitedFileList(
            wi, ['../a/afoo', 'bfoo'], msg="wait, relative create files")

        a_sub_dir = os.path.join(a_dir, 'asub')
        os.mkdir(a_sub_dir)
        b_sub_dir = os.path.join(b_dir, 'bsub')
        os.mkdir(b_sub_dir)
        self.assertWaitedFileList(
            wi, ['../a/asub', 'bsub'], msg="wait, relative create directories")
