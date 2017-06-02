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
try:
    import unittest2 as unittest
except ImportError:
    import unittest

import pywatchman


def is_root():
    return hasattr(os, 'geteuid') and os.geteuid() == 0

@WatchmanTestCase.expand_matrix
class TestPerms(WatchmanTestCase.WatchmanTestCase):

    def checkOSApplicability(self):
        if os.name == 'nt':
            self.skipTest('N/A on Windows')

    @unittest.skipIf(is_root(), "N/A if root")
    def test_permDeniedSubDir(self):
        root = self.mkdtemp()
        subdir = os.path.join(root, 'subdir')
        os.mkdir(subdir)
        os.chmod(subdir, 0)
        self.watchmanCommand('watch', root)
        res = self.watchmanCommand('query', root, {
            'expression': ['exists'],
            'fields': ['name']})
        self.assertRegexpMatches(res['warning'],
                                 'Marking this portion of the tree deleted')

    @unittest.skipIf(is_root(), "N/A if root")
    def test_permDeniedRoot(self):
        root = self.mkdtemp()
        os.chmod(root, 0)
        with self.assertRaisesRegexp(pywatchman.CommandError,
                '(open|opendir|realpath)'):
            self.watchmanCommand('watch', root)
