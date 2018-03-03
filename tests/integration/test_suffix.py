# vim:ts=4:sw=4:et:
# Copyright 2012-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import WatchmanTestCase
import pywatchman
import os


@WatchmanTestCase.expand_matrix
class TestMatch(WatchmanTestCase.WatchmanTestCase):
    def test_match_suffix(self):
        root = self.mkdtemp()
        self.touchRelative(root, 'foo.c')
        self.touchRelative(root, 'README.pdf')
        os.mkdir(os.path.join(root, 'html'))
        self.touchRelative(root, 'html', 'frames.html')
        self.touchRelative(root, 'html', 'mov.mp4')
        self.touchRelative(root, 'html', 'ignore.xxx')
        os.mkdir(os.path.join(root, 'win'))
        self.touchRelative(root, 'win', 'ms.dll')
        self.touchRelative(root, 'win', 'ignore.txt')

        self.watchmanCommand('watch', root)

        self.assertFileList(
            root, [
                'README.pdf', 'foo.c', 'html', 'win', 'win/ms.dll',
                'win/ignore.txt', 'html/frames.html', 'html/mov.mp4',
                'html/ignore.xxx'
            ]
        )
        # Simple anyof suffix query that watchman can convert to suffix array.
        # We will compare results against a user constructed suffix array query.
        res1 = self.watchmanCommand(
            'query', root, {
                'expression':
                ['anyof',
                    ['suffix', 'pdf'],
                    ['suffix', 'nomatch'],
                    ['suffix', 'dll'],
                 ],
                'fields': ['name']
            }
        )
        self.assertEqual(self.normWatchmanFileList(res1['files']),
                         self.normFileList(['README.pdf', 'win/ms.dll']))
        # User constructed anyof query with suffix array. This should give
        # same results as above query with list of suffixes.
        res2 = self.watchmanCommand(
            'query', root, {
                'expression':
                ['anyof',
                    ['suffix',
                        ['pdf', 'nomatch', 'dll']
                     ]
                 ],
                'fields': ['name']
            }
        )
        self.assertEqual(self.normWatchmanFileList(res1['files']),
                         self.normWatchmanFileList(res2['files']))
        # Another anyof suffix query that watchman can convert to suffix array.
        # This will check boundary (empty result set)
        res1 = self.watchmanCommand(
            'query', root, {
                'expression':
                ['anyof',
                    ['suffix', 'nomatch'],
                    ['suffix', 'none'],
                 ],
                'fields': ['name']
            }
        )
        self.assertEqual(self.normWatchmanFileList(res1['files']),
                         self.normFileList([]))
        # User constructed anyof query with suffix array. This should give
        # same results as above query with suffix array.
        res2 = self.watchmanCommand(
            'query', root, {
                'expression':
                ['anyof',
                    ['suffix', ['nomatch', 'none']]
                 ],
                'fields': ['name']
            }
        )
        self.assertEqual(self.normWatchmanFileList(res1['files']),
                         self.normWatchmanFileList(res2['files']))
        # Compound anyof suffix query that watchman can convert to suffix array.
        # We will compare results against suffix array query.
        res1 = self.watchmanCommand(
            'query', root, {
                'expression':
                ['anyof',
                    ['allof',
                        ['dirname', 'html'],
                        ['type', 'f'],
                        ['anyof',
                            ['suffix', 'pdf'],
                            ['suffix', 'html'],
                            ['suffix', 'nomatch'],
                         ]
                     ],
                    ['name', '.never-match-this', 'wholename'],
                 ],
                'fields': ['name']
            }
        )
        self.assertEqual(self.normWatchmanFileList(res1['files']),
                         self.normFileList(['html/frames.html']))
        # User constructed anyof suffix query. This should give same results
        # as above query with suffix array.
        res2 = self.watchmanCommand(
            'query', root, {
                'expression':
                ['anyof',
                    ['allof',
                        ['dirname', 'html'],
                        ['type', 'f'],
                        ['anyof',
                            ['suffix',
                                ['pdf', 'html', 'nomatch']
                             ]
                         ]
                     ],
                    ['name', '.never-match-this', 'wholename'],
                 ],
                'fields': ['name']
            }
        )
        self.assertEqual(self.normWatchmanFileList(res1['files']),
                         self.normWatchmanFileList(res2['files']))

    def test_suffix_expr(self):
        root = self.mkdtemp()

        self.touchRelative(root, 'foo.c')
        os.mkdir(os.path.join(root, 'subdir'))
        self.touchRelative(root, 'subdir', 'bar.txt')

        self.watchmanCommand('watch', root)
        self.assertFileListsEqual(
            self.watchmanCommand('query', root, {
                'expression': ['suffix', 'c'],
                'fields': ['name']})['files'],
            ['foo.c'])

        with self.assertRaises(pywatchman.WatchmanError) as ctx:
            self.watchmanCommand('query', root, {
                'expression': 'suffix'})

        self.assertRegexpMatches(
            str(ctx.exception),
            "Expected array for 'suffix' term")
