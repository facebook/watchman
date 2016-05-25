# vim:ts=4:sw=4:et:
# Copyright 2016-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0
import WatchmanTestCase
import tempfile
import os
import os.path
import sys
import unittest


class TestSymlink(WatchmanTestCase.WatchmanTestCase):

    # test to see that valid symbolic link
    # updates are picked up by the symlink_target field
    @unittest.skipIf(os.name == 'nt', 'win')
    def test_symlink(self):
        root = self.mkdtemp()
        self.touchRelative(root, '222')
        self.touchRelative(root, '333')
        os.symlink(os.path.join(root, '222'), os.path.join(root, '111'))

        self.watchmanCommand('watch', root)
        self.assertFileList(root, ['111', '222', '333'])

        expr = {
            "expression": ["name", "111"],
            "fields": ["name", "symlink_target"]
        }

        res = self.watchmanCommand('query', root, expr)
        self.assertEqual(os.path.basename(res['files'][0]['symlink_target']), '222')

        os.unlink(os.path.join(root, '111'))
        os.symlink(os.path.join(root, '333'), os.path.join(root, '111'))

        res = self.watchmanCommand('query', root, expr)
        self.assertEqual(os.path.basename(res['files'][0]['symlink_target']), '333')

    # test to see that invalid symbolic link
    # updates are picked up by the symlink_target field
    # Skipping this test on mac due to the dangling symlink bug in fsevents
    @unittest.skipIf(sys.platform == 'darwin' or os.name == 'nt', 'mac os or win')
    def test_invalidSymlink(self):
        root = self.mkdtemp()
        os.symlink(os.path.join(root, '222'), os.path.join(root, '111'))

        self.watchmanCommand('watch', root)
        self.assertFileList(root, ['111' ])

        expr = {
            "expression": ["name", "111"],
            "fields": ["name", "symlink_target"]
        }

        res = self.watchmanCommand('query', root, expr)
        self.assertEqual(os.path.basename(res['files'][0]['symlink_target']), '222')

        os.unlink(os.path.join(root, '111'))
        os.symlink(os.path.join(root, '333'), os.path.join(root, '111'))

        res = self.watchmanCommand('query', root, expr)
        self.assertEqual(os.path.basename(res['files'][0]['symlink_target']), '333')

    # test to see that symbolic link deletes work
    @unittest.skipIf(os.name == 'nt', 'win')
    def test_delSymlink(self):
        root = self.mkdtemp()
        self.touchRelative(root, '222')
        os.symlink(os.path.join(root, '222'), os.path.join(root, '111'))

        self.watchmanCommand('watch', root)
        self.assertFileList(root, ['111', '222' ])

        # Create a cursor for this state
        self.watchmanCommand('since', root, 'n:foo')

        os.unlink(os.path.join(root, '111'))

        expr = {
            "fields": ["name", "symlink_target"],
            "since" : 'n:foo',
        }

        res = self.watchmanCommand('query', root, expr)
        self.assertEqual(res['files'][0]['name'], '111')
        self.assertEqual(os.path.basename(res['files'][0]['symlink_target']), '222')

    # test to see that when a symbolic link is changed to a file,
    # the symlink target is updated correctly
    @unittest.skipIf(os.name == 'nt', 'win')
    def test_symlinkToFileDir(self):
        root = self.mkdtemp()
        self.touchRelative(root, '222')
        os.symlink(os.path.join(root, '222'), os.path.join(root, '111'))

        self.watchmanCommand('watch', root)
        self.assertFileList(root, ['111', '222'])

        expr = {
            "expression": ["name", "111"],
            "fields": ["name", "symlink_target", "type"]
        }

        res = self.watchmanCommand('query', root, expr)

        self.assertEqual(os.path.basename(res['files'][0]['symlink_target']), '222')
        self.assertEqual(res['files'][0]['type'], 'l')

        os.unlink(os.path.join(root, '111'))
        self.touchRelative(root, '111')

        res = self.watchmanCommand('query', root, expr)
        self.assertTrue(res['files'] and ('symlink_target' not in res['files'][0] or res['files'][0]['symlink_target'] is None))
        self.assertEqual(res['files'][0]['type'], 'f')

        os.unlink(os.path.join(root, '111'))
        os.mkdir(os.path.join(root, '111'))

        res = self.watchmanCommand('query', root, expr)
        self.assertTrue(res['files'] and ('symlink_target' not in res['files'][0] or res['files'][0]['symlink_target'] is None))
        self.assertEqual(res['files'][0]['type'], 'd')
