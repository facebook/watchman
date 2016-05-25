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

        res1 = self.watchmanCommand('query', root, expr)

        files1 = res1['files']
        self.assertTrue(files1 and os.path.basename(files1[0]['symlink_target']) == '222')

        os.unlink(os.path.join(root, '111'))
        os.symlink(os.path.join(root, '333'), os.path.join(root, '111'))

        res2 = self.watchmanCommand('query', root, expr)
        files2 = res2['files']
        #print files2
        self.assertTrue(files2 and os.path.basename(files2[0]['symlink_target']) == '333')

    # test to see that invalid symbolic link
    # updates are picked up by the symlink_target field
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

        res1 = self.watchmanCommand('query', root, expr)

        files1 = res1['files']
        self.assertTrue(files1 and os.path.basename(files1[0]['symlink_target']) == '222')

        os.unlink(os.path.join(root, '111'))
        os.symlink(os.path.join(root, '333'), os.path.join(root, '111'))

        res2 = self.watchmanCommand('query', root, expr)
        files2 = res2['files']
        #print files2
        self.assertTrue(files2 and os.path.basename(files2[0]['symlink_target']) == '333')

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
        files = res['files']
        #print files
        self.assertTrue(files and files[0]['name']  == '111'
            and os.path.basename(files[0]['symlink_target']) == '222')

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

        res1 = self.watchmanCommand('query', root, expr)

        files1 = res1['files']
        #print files1
        self.assertTrue(files1 and os.path.basename(files1[0]['symlink_target']) == '222'
                and files1[0]['type'] == 'l')

        os.unlink(os.path.join(root, '111'))
        self.touchRelative(root, '111')

        res2 = self.watchmanCommand('query', root, expr)
        files2 = res2['files']
        #print files2
        self.assertTrue(files2 and ('symlink_target' not in files2[0] or files2[0]['symlink_target'] is None)
                and files2[0]['type'] == 'f')

        os.unlink(os.path.join(root, '111'))
        os.mkdir(os.path.join(root, '111'))

        res3 = self.watchmanCommand('query', root, expr)
        files3 = res3['files']
        #print files3
        self.assertTrue(files3 and ('symlink_target' not in files3[0] or files3[0]['symlink_target'] is None)
                and files3[0]['type'] == 'd')
