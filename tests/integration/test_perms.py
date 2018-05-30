# vim:ts=4:sw=4:et:
# Copyright 2015-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

# no unicode literals
from __future__ import absolute_import, division, print_function

import os

import pywatchman
import WatchmanTestCase


try:
    import unittest2 as unittest
except ImportError:
    import unittest


def is_root():
    return hasattr(os, "geteuid") and os.geteuid() == 0


@WatchmanTestCase.expand_matrix
class TestPerms(WatchmanTestCase.WatchmanTestCase):
    def checkOSApplicability(self):
        if os.name == "nt":
            self.skipTest("N/A on Windows")

    @unittest.skipIf(is_root(), "N/A if root")
    def test_permDeniedSubDir(self):
        root = self.mkdtemp()
        subdir = os.path.join(root, "subdir")
        os.mkdir(subdir)
        os.chmod(subdir, 0)
        self.watchmanCommand("watch", root)
        res = self.watchmanCommand(
            "query", root, {"expression": ["exists"], "fields": ["name"]}
        )
        self.assertRegex(res["warning"], "Marking this portion of the tree deleted")

    @unittest.skipIf(is_root(), "N/A if root")
    def test_permDeniedRoot(self):
        root = self.mkdtemp()
        os.chmod(root, 0)
        with self.assertRaisesRegex(pywatchman.CommandError, "(open|opendir|realpath)"):
            self.watchmanCommand("watch", root)
