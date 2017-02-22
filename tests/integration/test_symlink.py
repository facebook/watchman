# vim:ts=4:sw=4:et:
# Copyright 2016-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0
import WatchmanTestCase
import tempfile
import os
import os.path
import sys
import json
try:
    import unittest2 as unittest
except ImportError:
    import unittest


def buggy_fsevents():
    ''' older versions of macOS had some problems reporting changes
        when a symlink was updated to a dangling value.  This was
        fixed in macOS 10.12 '''
    if sys.platform != 'darwin':
        return False
    # look at the kernel release; it is fixed in 16+
    return int(os.uname()[2].split('.')[0]) < 16


@WatchmanTestCase.expand_matrix
class TestSymlink(WatchmanTestCase.WatchmanTestCase):

    def checkOSApplicability(self):
        if os.name == 'nt':
            self.skipTest('N/A on Windows')

    def makeRootAndConfig(self):
        root = self.mkdtemp()
        with open(os.path.join(root, '.watchmanconfig'), 'w') as f:
            f.write(json.dumps({
                'watch_symlinks': True
            }))
        return root

    # test to see that valid symbolic link
    # updates are picked up by the symlink_target field
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
        self.assertEqual('222', os.path.basename(res['files'][0]['symlink_target']))

        os.unlink(os.path.join(root, '111'))
        os.symlink(os.path.join(root, '333'), os.path.join(root, '111'))

        res = self.watchmanCommand('query', root, expr)
        self.assertEqual('333', os.path.basename(res['files'][0]['symlink_target']))

    # test to see that invalid symbolic link
    # updates are picked up by the symlink_target field
    # Skipping this test on mac due to the dangling symlink bug in fsevents
    @unittest.skipIf(buggy_fsevents(), 'known fsevents bug')
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
        self.assertEqual(
            '222', os.path.basename(res['files'][0]['symlink_target'])
        )

        os.unlink(os.path.join(root, '111'))
        os.symlink(os.path.join(root, '333'), os.path.join(root, '111'))

        res = self.watchmanCommand('query', root, expr)
        self.assertEqual(
            '333', os.path.basename(res['files'][0]['symlink_target'])
        )

    # test to see that symbolic link deletes work
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
        self.assertEqual('111', res['files'][0]['name'])
        self.assertFalse(res['files'][0]['exists'])

    # test to see that when a symbolic link is changed to a file,
    # the symlink target is updated correctly
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

        self.assertEqual(
            '222', os.path.basename(res['files'][0]['symlink_target'])
        )
        self.assertEqual('l', res['files'][0]['type'])

        os.unlink(os.path.join(root, '111'))
        self.touchRelative(root, '111')

        res = self.watchmanCommand('query', root, expr)
        self.assertTrue(res['files'] and ('symlink_target' not in res['files'][0] or res['files'][0]['symlink_target'] is None))
        self.assertEqual('f', res['files'][0]['type'])

        os.unlink(os.path.join(root, '111'))
        os.mkdir(os.path.join(root, '111'))

        res = self.watchmanCommand('query', root, expr)
        self.assertTrue(res['files'] and ('symlink_target' not in res['files'][0] or res['files'][0]['symlink_target'] is None))
        self.assertEqual('d', res['files'][0]['type'])

    def test_watchSymlinkTarget(self):
        def make_roots():
            # B/link -> C/file
            # A/link -> B/file
            rootA = self.makeRootAndConfig()
            rootB = self.makeRootAndConfig()
            rootC = self.makeRootAndConfig()
            self.touchRelative(rootA, 'file')
            self.touchRelative(rootB, 'file')
            self.touchRelative(rootC, 'file')
            os.symlink(os.path.join(rootC, 'file'), os.path.join(rootB, 'link'))
            os.symlink(os.path.join(rootB, 'file'), os.path.join(rootA, 'link'))
            return rootA, rootB, rootC

        root1, root2, root3 = make_roots()
        root1prime, root2prime, root3prime = make_roots()
        self.watchmanCommand('watch', root1)
        expected = [root1, root2, root3]
        self.assertWatchListContains(expected)
        self.assertFileList(root1, ['.watchmanconfig', 'file', 'link'])

        os.symlink(os.path.join(root1prime, 'file'),
                   os.path.join(root1, 'linkToPrime'))
        expected += [root1prime, root2prime, root3prime]
        self.assertWatchListContains(expected)
        self.assertFileList(root1prime, ['.watchmanconfig', 'file', 'link'])

    def test_watchSymlinkTargetLinkToLink(self):
        def make_roots():
            # B/link -> C/file
            # A/link -> B/link
            rootA = self.makeRootAndConfig()
            rootB = self.makeRootAndConfig()
            rootC = self.makeRootAndConfig()
            self.touchRelative(rootA, 'file')
            self.touchRelative(rootC, 'file')
            os.symlink(os.path.join(rootC, 'file'), os.path.join(rootB, 'link'))
            os.symlink(os.path.join(rootB, 'link'), os.path.join(rootA, 'link'))
            return rootA, rootB, rootC

        root1, root2, root3 = make_roots()
        root1prime, root2prime, root3prime = make_roots()
        self.watchmanCommand('watch', root1)
        expected = [root1, root2, root3]
        self.assertWatchListContains(expected)
        self.assertFileList(root1, ['.watchmanconfig', 'file', 'link'])

        os.symlink(os.path.join(root1prime, 'file'),
                   os.path.join(root1, 'linkToPrime'))
        expected += [root1prime, root2prime, root3prime]
        self.assertWatchListContains(expected)
        self.assertFileList(root1prime, ['.watchmanconfig', 'file', 'link'])

    def test_watchRelativeSymlinkTarget(self):
        def make_roots():
            # A/link --relative-link--> B/file
            # So, the target of A/link is "../B/file"
            rootA = self.makeRootAndConfig()
            rootB = self.makeRootAndConfig()
            self.touchRelative(rootA, 'file')
            self.touchRelative(rootB, 'file')
            os.symlink(os.path.relpath(os.path.join(rootB, 'file'),
                                       rootA),
                       os.path.join(rootA, 'link'))
            return rootA, rootB

        root1, root2 = make_roots()
        root1prime, root2prime = make_roots()
        self.watchmanCommand('watch', root1)
        expected = [root1, root2]
        self.assertWatchListContains(expected)
        self.assertFileList(root1, ['.watchmanconfig', 'file', 'link'])

        os.symlink(os.path.join(root1prime, 'file'),
                   os.path.join(root1, 'linkToPrime'))
        expected += [root1prime, root2prime]
        self.assertWatchListContains(expected)
        self.assertFileList(root1prime, ['.watchmanconfig', 'file', 'link'])

    def test_watchRelativeSymlinkTargetOneHop(self):
        def make_roots():
            # A/dir/link --relative-link--> B/file
            # So, the target of A/dir/link is "../../B/file"
            rootA = self.makeRootAndConfig()
            rootB = self.makeRootAndConfig()
            self.touchRelative(rootA, 'file')
            os.mkdir(os.path.join(rootA, 'dir'))
            self.touchRelative(rootB, 'file')
            os.symlink(os.path.relpath(os.path.join(rootB, 'file'),
                                       os.path.join(rootA, 'dir')),
                       os.path.join(rootA, 'dir', 'link'))
            return rootA, rootB

        root1, root2 = make_roots()
        root1prime, root2prime = make_roots()
        self.watchmanCommand('watch-project', root1)
        expected = [root1, root2]
        self.assertWatchListContains(expected)
        self.assertFileList(root1, ['.watchmanconfig', 'file', 'dir', 'dir/link'])

        os.symlink(os.path.join(root1prime, 'file'),
                   os.path.join(root1, 'linkToPrime'))
        expected += [root1prime, root2prime]
        self.assertWatchListContains(expected)
        self.assertFileList(root1prime, ['.watchmanconfig', 'file', 'dir', 'dir/link'])

    def test_watchSymlinkToDir(self):
        def make_roots():
            # A/link -> B/dir
            rootA = self.makeRootAndConfig()
            rootB = self.makeRootAndConfig()
            os.mkdir(os.path.join(rootB, 'dir'))
            self.touchRelative(os.path.join(rootB, 'dir'), 'file')
            os.symlink(os.path.join(rootB, 'dir', 'file'),
                       os.path.join(rootA, 'linkToDir'))
            self.touchRelative(rootA, 'file')
            return rootA, rootB

        root1, root2 = make_roots()
        root1prime, root2prime = make_roots()
        self.watchmanCommand('watch-project', root1)
        expected = [root1, root2]
        self.assertWatchListContains(expected)
        self.assertFileList(root1, ['.watchmanconfig', 'file', 'linkToDir'])

        os.symlink(os.path.join(root1prime, 'file'),
                   os.path.join(root1, 'linkToPrime'))
        expected += [root1prime, root2prime]
        self.assertWatchListContains(expected)
        self.assertFileList(root1prime, ['.watchmanconfig', 'file', 'linkToDir'])

    def test_watchSymlinkToDirContainsSymlink(self):
        def make_roots():
            # B/dir/link -> C/file
            # A/link -> B/dir
            rootA = self.makeRootAndConfig()
            rootB = self.makeRootAndConfig()
            rootC = self.makeRootAndConfig()
            self.touchRelative(rootC, 'file')
            os.mkdir(os.path.join(rootB, 'dir'))
            os.symlink(os.path.join(rootC, 'file'),
                       os.path.join(rootB, 'dir', 'link'))
            os.symlink(os.path.join(rootB, 'dir'),
                       os.path.join(rootA, 'linkToDir'))
            self.touchRelative(os.path.join(rootA, 'file'))
            return rootA, rootB, rootC

        root1, root2, root3 = make_roots()
        root1prime, root2prime, root3prime = make_roots()
        self.watchmanCommand('watch-project', root1)
        expected = [root1, root2, root3]
        self.assertWatchListContains(expected)
        self.assertFileList(root1, ['.watchmanconfig', 'file', 'linkToDir'])

        os.symlink(os.path.join(root1prime, 'file'),
                   os.path.join(root1, 'linkToPrime'))
        expected += [root1prime, root2prime, root3prime]
        self.assertWatchListContains(expected)
        self.assertFileList(root1prime, ['.watchmanconfig', 'file', 'linkToDir'])
