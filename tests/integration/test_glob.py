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
import pywatchman


@WatchmanTestCase.expand_matrix
class TestGlob(WatchmanTestCase.WatchmanTestCase):

    def test_glob(self):
        root = self.mkdtemp()
        self.touchRelative(root, 'a.c')
        self.touchRelative(root, 'b.c')
        self.touchRelative(root, '.a.c')

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
        self.assertEqual(self.normFileList(['a.h', 'b.h']),
                         self.normWatchmanFileList(res['files']))

        res = self.watchmanCommand('query', root, {
            'glob': ['**/*.h'],
            'fields': ['name']})
        self.assertEqual(self.normFileList(['includes/a.h', 'includes/b.h',
                                            'includes/second/bar.h',
                                            'includes/second/foo.h']),
                         self.normWatchmanFileList(res['files']))

        res = self.watchmanCommand('query', root, {
            'glob': ['**/*.h'],
            'relative_root': 'includes',
            'fields': ['name']})
        self.assertEqual(self.normFileList(['a.h', 'b.h',
                                            'second/bar.h',
                                            'second/foo.h']),
                         self.normWatchmanFileList(res['files']))

        res = self.watchmanCommand('query', root, {
            'glob': ['*.c'],
            'fields': ['name']})
        self.assertEqual(self.normWatchmanFileList(res['files']),
                         self.normFileList(['a.c', 'b.c']))

        res = self.watchmanCommand('query', root, {
            'glob': ['*.c'],
            'glob_includedotfiles': True,
            'fields': ['name']})
        self.assertEqual(self.normWatchmanFileList(res['files']),
                         self.normFileList(['.a.c', 'a.c', 'b.c']))

        res = self.watchmanCommand('query', root, {
            'glob': ['**/*.h', '**/**/*.h'],
            'fields': ['name']})
        self.assertEqual(self.normFileList(['includes/a.h', 'includes/b.h',
                                            'includes/second/bar.h',
                                            'includes/second/foo.h']),
                         self.normWatchmanFileList(res['files']))

        # check that dedup is happening
        res = self.watchmanCommand('query', root, {
            'glob': ['**/*.h', '**/**/*.h', 'includes/*.h'],
            'fields': ['name']})
        self.assertEqual(self.normFileList(['includes/a.h', 'includes/b.h',
                                            'includes/second/bar.h',
                                            'includes/second/foo.h']),
                         self.normWatchmanFileList(res['files']))

        shutil.rmtree(second_inc_dir)

        res = self.watchmanCommand('query', root, {
            'glob': ['**/*.h', '**/**/*.h'],
            'fields': ['name']})
        self.assertEqual(self.normFileList(['includes/a.h', 'includes/b.h']),
                         self.normWatchmanFileList(res['files']))

        res = self.watchmanCommand('query', root, {
            'glob': ['*/*.h'],
            'fields': ['name']})
        self.assertEqual(self.normFileList(['includes/a.h', 'includes/b.h']),
                         self.normWatchmanFileList(res['files']))

        os.unlink(os.path.join(inc_dir, 'a.h'))

        res = self.watchmanCommand('query', root, {
            'glob': ['*/*.h'],
            'fields': ['name']})
        self.assertEqual(self.normFileList(['includes/b.h']),
                         self.normWatchmanFileList(res['files']))

        with self.assertRaises(pywatchman.WatchmanError) as ctx:
            self.watchmanCommand('query', root, {
                'glob': ['*/*.h'],
                'relative_root': 'bogus'})
        self.assertIn('check your relative_root', str(ctx.exception))

        with self.assertRaises(pywatchman.WatchmanError) as ctx:
            self.watchmanCommand('query', root, {
                'glob': [12345]})
        self.assertIn('expected json string object', str(ctx.exception))
