# vim:ts=4:sw=4:et:
# Copyright 2016-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0
import WatchmanTestCase
import tempfile
import os
import os.path
import sys
try:
    import unittest2 as unittest
except ImportError:
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
        self.assertEqualUTF8Strings('222', os.path.basename(res['files'][0]['symlink_target']))

        os.unlink(os.path.join(root, '111'))
        os.symlink(os.path.join(root, '333'), os.path.join(root, '111'))

        res = self.watchmanCommand('query', root, expr)
        self.assertEqualUTF8Strings('333', os.path.basename(res['files'][0]['symlink_target']))

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
        self.assertEqualUTF8Strings('222', os.path.basename(res['files'][0]['symlink_target']))

        os.unlink(os.path.join(root, '111'))
        os.symlink(os.path.join(root, '333'), os.path.join(root, '111'))

        res = self.watchmanCommand('query', root, expr)
        self.assertEqualUTF8Strings('333', os.path.basename(res['files'][0]['symlink_target']))

    # test to see that symbolic link deletes work
    @unittest.skipIf(os.name == 'nt', 'win')
    def test_delSymlink(self):
        root = self.mkdtemp()
        self.touchRelative(root, '222')
        os.symlink(os.path.join(root, '222'), os.path.join(root, '111'))

        self.watchmanCommand('watch', root)
        self.assertFileList(root, ['111', '222' ])

        # capture the clock so that we can use a since query,
        # see the removed entry when we query later on.
        clock  = self.watchmanCommand('clock', root)['clock']

        os.unlink(os.path.join(root, '111'))

        expr = {
            "expression" : [ "name", "111"],
            "fields": ["name", "exists"],
            "since" : clock
        }

        res = self.watchmanCommand('query', root, expr)
        self.assertEqualUTF8Strings('111', res['files'][0]['name'])
        self.assertFalse(res['files'][0]['exists'])

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

        self.assertEqualUTF8Strings('222', os.path.basename(res['files'][0]['symlink_target']))
        self.assertEqualUTF8Strings('l', res['files'][0]['type'])

        os.unlink(os.path.join(root, '111'))
        self.touchRelative(root, '111')

        res = self.watchmanCommand('query', root, expr)
        self.assertTrue(res['files'] and ('symlink_target' not in res['files'][0] or res['files'][0]['symlink_target'] is None))
        self.assertEqualUTF8Strings('f', res['files'][0]['type'])

        os.unlink(os.path.join(root, '111'))
        os.mkdir(os.path.join(root, '111'))

        res = self.watchmanCommand('query', root, expr)
        self.assertTrue(res['files'] and ('symlink_target' not in res['files'][0] or res['files'][0]['symlink_target'] is None))
        self.assertEqualUTF8Strings('d', res['files'][0]['type'])
