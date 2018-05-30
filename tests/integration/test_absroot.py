# vim:ts=4:sw=4:et:
# Copyright 2014-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

# no unicode literals
from __future__ import absolute_import, division, print_function

import os
import os.path

import pywatchman
import WatchmanTestCase


@WatchmanTestCase.expand_matrix
class TestAbsoluteRoot(WatchmanTestCase.WatchmanTestCase):
    def test_dot(self):
        root = self.mkdtemp()

        save_dir = os.getcwd()
        try:
            os.chdir(root)

            dot = "" if os.name == "nt" else "."

            if self.transport == "cli":
                res = self.watchmanCommand("watch", dot)
                self.assertEqual(root, self.normAbsolutePath(res["watch"]))
            else:
                with self.assertRaises(pywatchman.WatchmanError) as ctx:
                    self.watchmanCommand("watch", dot)

                self.assertIn("must be absolute", str(ctx.exception))

        finally:
            os.chdir(save_dir)

    def test_root(self):
        if os.name != "nt":
            with self.assertRaises(pywatchman.WatchmanError) as ctx:
                self.watchmanCommand("watch", "/")

                self.assertIn("cannot watch", str(ctx.exception))
