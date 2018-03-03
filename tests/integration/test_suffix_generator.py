# vim:ts=4:sw=4:et:
# Copyright 2012-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import WatchmanTestCase
import os
import pywatchman


@WatchmanTestCase.expand_matrix
class TestSuffixGenerator(WatchmanTestCase.WatchmanTestCase):
    def test_suffix_generator(self):
        root = self.mkdtemp()

        self.touchRelative(root, 'foo.c')
        os.mkdir(os.path.join(root, 'subdir'))
        self.touchRelative(root, 'subdir', 'bar.txt')

        self.watchmanCommand('watch', root)

        self.assertFileListsEqual(
            self.watchmanCommand('query', root, {
                'suffix': 'c',
                'fields': ['name']})['files'],
            ['foo.c'])

        self.assertFileListsEqual(
            self.watchmanCommand('query', root, {
                'suffix': ['c', 'txt'],
                'fields': ['name']})['files'],
            ['foo.c', 'subdir/bar.txt'])

        self.assertFileListsEqual(
            self.watchmanCommand('query', root, {
                'suffix': ['c', 'txt'],
                'relative_root': 'subdir',
                'fields': ['name']})['files'],
            ['bar.txt'])

        with self.assertRaises(pywatchman.WatchmanError) as ctx:
            self.watchmanCommand('query', root, {
                'suffix': {'a': 'b'}})

        self.assertRegexpMatches(
            str(ctx.exception),
            "'suffix' must be a string or an array of strings")
