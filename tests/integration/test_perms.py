# vim:ts=4:sw=4:et:
# Copyright 2015-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import WatchmanTestCase
import tempfile
import os
import os.path
import unittest
import pywatchman


class TestPerms(WatchmanTestCase.WatchmanTestCase):

    @unittest.skipIf(os.name == 'nt' or os.geteuid() == 0, "win or root")
    def test_permDeniedSubDir(self):
        root = self.mkdtemp()
        subdir = os.path.join(root, 'subdir')
        os.mkdir(subdir)
        os.chmod(subdir, 0)
        self.watchmanCommand('watch', root)
        res = self.watchmanCommand('query', root, {
            'expression': ['exists'],
            'fields': ['name']})
        warning = self.decodeBSERUTF8(res['warning'])
        self.assertRegexpMatches(warning,
                                 'Marking this portion of the tree deleted')

    @unittest.skipIf(os.name == 'nt' or os.geteuid() == 0, "win or root")
    def test_permDeniedRoot(self):
        root = self.mkdtemp()
        os.chmod(root, 0)
        with self.assertRaisesRegexp(pywatchman.CommandError, 'failed to opendir'):
            self.watchmanCommand('watch', root)
