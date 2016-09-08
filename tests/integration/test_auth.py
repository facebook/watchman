# vim:ts=4:sw=4:et:
# Copyright 2016-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import WatchmanTestCase
import pywatchman


@WatchmanTestCase.expand_matrix
class TestAuth(WatchmanTestCase.WatchmanTestCase):
    def requiresPersistentSession(self):
        return True

    def test_dropPriv(self):
        root = self.mkdtemp()
        self.touchRelative(root, '111')

        self.watchmanCommand('watch', root)

        # pretend we are not the owner
        self.watchmanCommand('debug-drop-privs')

        # Should be able to watch something that is already watched
        self.watchmanCommand('watch', root)

        # can't make a new watch
        altroot = self.mkdtemp()
        with self.assertRaises(pywatchman.WatchmanError):
            self.watchmanCommand('watch', altroot)

        # Should not be able to delete a watch
        with self.assertRaises(pywatchman.WatchmanError):
            self.watchmanCommand('watch-del', root)

        # or register a trigger
        with self.assertRaises(pywatchman.WatchmanError):
            self.watchmanCommand('trigger', root, 'trig', '*.js', '--', 'false')
