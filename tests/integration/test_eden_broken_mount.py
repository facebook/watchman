# vim:ts=4:sw=4:et:
# Copyright 2012-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

# no unicode literals
from __future__ import absolute_import, division, print_function

import os

import pywatchman
import WatchmanEdenTestCase
import WatchmanTestCase


@WatchmanTestCase.expand_matrix
class TestBrokenEden(WatchmanTestCase.WatchmanTestCase):

    def test_broken_eden(self):
        if not WatchmanEdenTestCase.can_run_eden():
            self.skipTest("eden not available")

        root = self.mkdtemp()

        # fake up a .eden dir
        dot_eden = os.path.join(root, ".eden")
        os.mkdir(dot_eden)
        os.symlink(root, os.path.join(dot_eden, "root"))
        os.symlink("fake!", os.path.join(dot_eden, "socket"))

        with self.assertRaises(pywatchman.CommandError) as ctx:
            self.watchmanCommand("watch", root)
        self.assertRegex(str(ctx.exception), "failed to communicate with eden mount")
