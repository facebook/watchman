# vim:ts=4:sw=4:et:
# Copyright 2018-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import WatchmanTestCase
import os
import pywatchman


@WatchmanTestCase.expand_matrix
class TestCookie(WatchmanTestCase.WatchmanTestCase):

    def test_delete_cookie_dir(self):
        root = self.mkdtemp()
        cookie_dir = os.path.join(root, '.hg')
        os.mkdir(cookie_dir)
        self.touchRelative(root, 'foo')

        self.watchmanCommand('watch-project', root)
        self.assertFileList(root, files=['foo', '.hg'])

        os.rmdir(cookie_dir)
        self.assertFileList(root, files=['foo'])
        os.unlink(os.path.join(root, 'foo'))
        self.assertFileList(root, files=[])
        os.rmdir(root)
        with self.assertRaises(pywatchman.WatchmanError) as ctx:
            self.assertFileList(root, files=[])
        reason = str(ctx.exception)
        self.assertTrue(
            ('No such file' in reason) or
            ('unable to resolve root' in reason),
            msg=reason)
