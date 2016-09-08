# vim:ts=4:sw=4:et:
# Copyright 2012-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import WatchmanTestCase
import tempfile
import os
import os.path


@WatchmanTestCase.expand_matrix
class TestMatch(WatchmanTestCase.WatchmanTestCase):

    def test_match(self):
        root = self.mkdtemp()
        self.touchRelative(root, 'foo.c')
        self.touchRelative(root, 'bar.txt')
        os.mkdir(os.path.join(root, 'foo'))
        self.touchRelative(root, 'foo', '.bar.c')
        self.touchRelative(root, 'foo', 'baz.c')

        self.watchmanCommand('watch', root)

        self.assertFileList(root, ['bar.txt', 'foo.c', 'foo', 'foo/.bar.c', 'foo/baz.c'])

        res = self.watchmanCommand('query', root, {
          'expression': ['match', '*.c'],
          'fields': ['name']})
        self.assertEqual(self.normWatchmanFileList(res['files']),
                         self.normFileList(['foo.c', 'foo/baz.c']))

        res = self.watchmanCommand('query', root, {
          'expression': ['match', '*.c', 'wholename'],
          'fields': ['name']})
        self.assertEqual(self.normWatchmanFileList(res['files']),
                         self.normFileList(['foo.c']))

        res = self.watchmanCommand('query', root, {
          'expression': ['match', 'foo/*.c', 'wholename'],
          'fields': ['name']})
        self.assertEqual(self.normWatchmanFileList(res['files']),
                         self.normFileList(['foo/baz.c']))

        res = self.watchmanCommand('query', root, {
          'expression': ['match', 'foo/*.c', 'wholename'],
          'fields': ['name']})
        self.assertEqual(self.normWatchmanFileList(res['files']),
                         self.normFileList(['foo/baz.c']))

        res = self.watchmanCommand('query', root, {
          'expression': ['match', '**/*.c', 'wholename'],
          'fields': ['name']})
        self.assertEqual(self.normWatchmanFileList(res['files']),
                         self.normFileList(['foo.c', 'foo/baz.c']))

        res = self.watchmanCommand('query', root, {
          'expression': ['match', '**/*.c', 'wholename', {'includedotfiles':True}],
          'fields': ['name']})
        self.assertEqual(self.normWatchmanFileList(res['files']),
                         self.normFileList(['foo.c', 'foo/.bar.c', 'foo/baz.c']))

        res = self.watchmanCommand('query', root, {
          'expression': ['match', 'foo/**/*.c', 'wholename'],
          'fields': ['name']})
        self.assertEqual(self.normWatchmanFileList(res['files']),
                         self.normFileList(['foo/baz.c']))

        res = self.watchmanCommand('query', root, {
            'expression': ['match', 'FOO/*.c', 'wholename'],
            'fields': ['name']})
        if self.isCaseInsensitive():
            self.assertEqual(self.normWatchmanFileList(res['files']),
                            self.normFileList(['foo/baz.c']))
        else:
            self.assertEqual(self.normWatchmanFileList(res['files']), [])

        res = self.watchmanCommand('query', root, {
            'expression': ['match', 'FOO/*.c', 'wholename'],
            'case_sensitive': True,
            'fields': ['name']})
        self.assertEqual(self.normWatchmanFileList(res['files']), [])

        res = self.watchmanCommand('query', root, {
            'expression': ['match', 'FOO/*.c', 'wholename'],
            'case_sensitive': False,
            'fields': ['name']})
        self.assertEqual(self.normWatchmanFileList(res['files']),
                            self.normFileList(['foo/baz.c']))
