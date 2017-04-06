# vim:ts=4:sw=4:et:
# Copyright 2017-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import WatchmanEdenTestCase


class TestEdenSubscribe(WatchmanEdenTestCase.WatchmanEdenTestCase):
    def requiresPersistentSession(self):
        return True

    def test_eden_subscribe(self):
        def populate(repo):
            repo.write_file('hello', 'hola\n')
            repo.commit('initial commit.')

        root = self.makeEdenMount(populate)

        res = self.watchmanCommand('watch', root)
        self.assertEqual('eden', res['watcher'])

        self.watchmanCommand('subscribe', root, 'myname', {'fields': ['name']})

        dat = self.waitForSub('myname', root=root)[0]
        self.assertTrue(dat['is_fresh_instance'])
        self.assertFileListsEqual(self.normWatchmanFileList(dat['files']),
                                  self.normFileList(['.eden', '.eden/socket',
                                                     '.eden/client',
                                                     '.eden/root', 'hello']))

        self.touchRelative(root, 'w0000t')
        dat = self.waitForSub('myname', root=root)[0]
        self.assertEqual(False, dat['is_fresh_instance'])
        self.assertFileListsEqual(self.normWatchmanFileList(dat['files']),
                                  self.normFileList(['w0000t']))

        self.touchRelative(root, 'hello')
        dat = self.waitForSub('myname', root=root)[0]
        self.assertEqual(False, dat['is_fresh_instance'])
        self.assertFileListsEqual(self.normWatchmanFileList(dat['files']),
                                  self.normFileList(['hello']))

        # make another subscription and assert that we get a fresh
        # instance result with all the files in it
        self.watchmanCommand('subscribe', root, 'othersub', {
            'fields': ['name']})

        dat = self.waitForSub('othersub', root=root)[0]
        self.assertEqual(True, dat['is_fresh_instance'])
        self.assertFileListsEqual(self.normWatchmanFileList(dat['files']),
                                  self.normFileList(['.eden', '.eden/socket',
                                                     '.eden/client',
                                                     '.eden/root', 'hello',
                                                     'w0000t']))
