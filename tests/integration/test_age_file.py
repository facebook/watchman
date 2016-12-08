# vim:ts=4:sw=4:et:
# Copyright 2012-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import WatchmanTestCase
import os
import os.path
import shutil


@WatchmanTestCase.expand_matrix
class TestAgeOutFile(WatchmanTestCase.WatchmanTestCase):
    def test_age_file(self):
        root = self.mkdtemp()

        os.mkdir(os.path.join(root, 'a'))
        self.touchRelative(root, 'a', 'file.txt')
        self.touchRelative(root, 'b.txt')

        self.watchmanCommand('watch', root)
        self.assertFileList(root, ['a', 'a/file.txt', 'b.txt'])

        res = self.watchmanCommand(
            'query', root, {'fields': ['name', 'exists']}
        )
        self.assertTrue(res['is_fresh_instance'])
        clock = res['clock']

        # Removing file nodes also impacts the suffix list, so we test
        # that it is operating as intended in here too
        res = self.watchmanCommand(
            'query', root, {
                'expression': ['suffix', 'txt'],
                'fields': ['name']
            }
        )

        self.assertFileListsEqual(res['files'], ['a/file.txt', 'b.txt'])

        # Let's track a named cursor; we need to validate that it is
        # correctly aged out
        self.watchmanCommand('since', root, 'n:foo')
        cursors = self.watchmanCommand('debug-show-cursors', root)
        self.assertTrue('n:foo' in cursors['cursors'])

        os.unlink(os.path.join(root, 'a', 'file.txt'))
        shutil.rmtree(os.path.join(root, 'a'))

        self.assertFileList(root, ['b.txt'])

        # Prune all deleted items
        self.watchmanCommand('debug-ageout', root, 0)

        # Wait for 'a' to age out and cause is_fresh_instance to be set
        def is_fresh():
            res = self.watchmanCommand(
                'query', root, {
                    'since': clock,
                    'fields': ['name', 'exists']
                }
            )
            return res.get('is_fresh_instance', False)

        self.waitFor(lambda: is_fresh())

        self.assertFileList(root, ['b.txt'], cursor=clock)

        # Our cursor should have been collected
        cursors = self.watchmanCommand('debug-show-cursors', root)
        self.assertFalse('n:foo' in cursors['cursors'])

        # Add a new file to the suffix list; this will insert at the head
        self.touchRelative(root, 'c.txt')

        # Suffix quere to very that linkage is safe
        res = self.watchmanCommand(
            'query', root, {
                'expression': ['suffix', 'txt'],
                'fields': ['name']
            }
        )
        self.assertFileListsEqual(
            ['b.txt', 'c.txt'], self.normFileList(res['files'])
        )

        # Stress the aging a bit
        for i in range(3):
            os.mkdir(os.path.join(root, 'dir'))
            for j in range(100):
                self.touchRelative(root, 'stress-%d' % j)
                self.touchRelative(root, 'dir', str(j))

            for j in range(100):
                os.unlink(os.path.join(root, 'stress-%d' % j))

            shutil.rmtree(os.path.join(root, 'dir'))

            self.assertFileList(root, ['b.txt', 'c.txt'])
            self.watchmanCommand('debug-ageout', root, 0)
            self.assertFileList(root, ['b.txt', 'c.txt'])
