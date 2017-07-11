# vim:ts=4:sw=4:et:
# Copyright 2016-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import WatchmanEdenTestCase
import os


class TestEdenSince(WatchmanEdenTestCase.WatchmanEdenTestCase):
    def test_eden_since(self):
        def populate(repo):
            repo.write_file('hello', 'hola\n')
            repo.write_file('adir/file', 'foo!\n')
            repo.write_file('bdir/test.sh', '#!/bin/bash\necho test\n',
                                mode=0o755)
            repo.write_file('bdir/noexec.sh', '#!/bin/bash\necho test\n')
            repo.symlink('slink', 'hello')
            repo.commit('initial commit.')

        root = self.makeEdenMount(populate)

        res = self.watchmanCommand('watch', root)
        self.assertEqual('eden', res['watcher'])

        res = self.watchmanCommand('query', root, {
            'expression': ['type', 'f'],
            'fields': ['name'],
            'since': 'c:0:0'})
        self.assertTrue(res['is_fresh_instance'])
        self.assertFileListsEqual(
            res['files'], ['hello', 'adir/file', 'bdir/test.sh', 'bdir/noexec.sh'])

        res = self.watchmanCommand('query', root, {
            'expression': ['type', 'f'],
            'relative_root': 'adir',
            'fields': ['name'],
            'since': 'c:0:0'})

        self.assertFileListsEqual(
            res['files'], ['file'],
            message='should only return adir/file with no adir prefix')

        clock = res['clock']

        self.touchRelative(root, 'hello')
        res = self.watchmanCommand('query', root, {
            'expression': ['type', 'f'],
            'fields': ['name'],
            'since': clock})
        self.assertFileListsEqual(res['files'], ['hello'])

        res = self.watchmanCommand('query', root, {
            'expression': ['type', 'f'],
            'fields': ['name'],
            'since': res['clock']})
        self.assertFileListsEqual(res['files'], [])

        res = self.watchmanCommand('query', root, {
            'expression': ['type', 'f'],
            'empty_on_fresh_instance': True,
            'fields': ['name'],
            'since': 'c:0:0'})
        self.assertTrue(res['is_fresh_instance'])
        self.assertFileListsEqual(res['files'], [])

        os.unlink(os.path.join(root, 'hello'))
        res = self.watchmanCommand('query', root, {
            'fields': ['name'],
            'since': res['clock']})
        self.assertFileListsEqual(res['files'], ['hello'])

        res = self.watchmanCommand('query', root, {
            'expression': ['type', 'f'],
            'fields': ['name'],
            'since': res['clock']})
        self.assertFileListsEqual(res['files'], [])
