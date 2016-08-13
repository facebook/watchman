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
import shutil


class TestGlob(WatchmanTestCase.WatchmanTestCase):

    def test_glob(self):
        root = self.mkdtemp()
        self.touchRelative(root, 'a.c')
        self.touchRelative(root, 'b.c')

        inc_dir = os.path.join(root, 'includes')
        os.mkdir(inc_dir)
        self.touchRelative(inc_dir, 'a.h')
        self.touchRelative(inc_dir, 'b.h')

        second_inc_dir = os.path.join(inc_dir, 'second')
        os.mkdir(second_inc_dir)
        self.touchRelative(second_inc_dir, 'foo.h')
        self.touchRelative(second_inc_dir, 'bar.h')

        self.watchmanCommand('watch', root)

        res = self.watchmanCommand('query', root, {
            'glob': ['*.h'],
            'fields': ['name']})
        self.assertEqual(res['files'], [])

        res = self.watchmanCommand('query', root, {
            'glob': ['*.h'],
            'relative_root': 'includes',
            'fields': ['name']})
        self.assertEqual(self.normWatchmanFileList(res['files']),
                         self.normFileList(['a.h', 'b.h']))

        res = self.watchmanCommand('query', root, {
            'glob': ['**/*.h'],
            'fields': ['name']})
        self.assertEqual(self.normWatchmanFileList(res['files']),
                         self.normFileList(['includes/a.h', 'includes/b.h',
                                            'includes/second/bar.h',
                                            'includes/second/foo.h']))

        res = self.watchmanCommand('query', root, {
            'glob': ['**/*.h'],
            'relative_root': 'includes',
            'fields': ['name']})
        self.assertEqual(self.normWatchmanFileList(res['files']),
                         self.normFileList(['second/bar.h',
                                            'second/foo.h']))

        res = self.watchmanCommand('query', root, {
            'glob': ['*.c'],
            'fields': ['name']})
        self.assertEqual(self.normWatchmanFileList(res['files']),
                         self.normFileList(['a.c', 'b.c']))

        res = self.watchmanCommand('query', root, {
            'glob': ['**/*.h', '**/**/*.h'],
            'fields': ['name']})
        self.assertEqual(self.normWatchmanFileList(res['files']),
                         self.normFileList(['includes/a.h', 'includes/b.h',
                                            'includes/second/bar.h',
                                            'includes/second/foo.h']))

        shutil.rmtree(second_inc_dir)

        res = self.watchmanCommand('query', root, {
            'glob': ['**/*.h', '**/**/*.h'],
            'fields': ['name']})
        self.assertEqual(self.normWatchmanFileList(res['files']),
                         self.normFileList(['includes/a.h', 'includes/b.h']))

        res = self.watchmanCommand('query', root, {
            'glob': ['*/*.h'],
            'fields': ['name']})
        self.assertEqual(self.normWatchmanFileList(res['files']),
                         self.normFileList(['includes/a.h', 'includes/b.h']))

        os.unlink(os.path.join(inc_dir, 'a.h'))

        res = self.watchmanCommand('query', root, {
            'glob': ['*/*.h'],
            'fields': ['name']})
        self.assertEqual(self.normWatchmanFileList(res['files']),
                         self.normFileList(['includes/b.h']))

