# vim:ts=4:sw=4:et:
# Copyright 2016-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import WatchmanEdenTestCase


class TestEdenPathGenerator(WatchmanEdenTestCase.WatchmanEdenTestCase):
    def test_eden_watch(self):
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
        self.assertFileList(root, ['adir', 'adir/file', 'bdir',
                                   'bdir/noexec.sh', 'bdir/test.sh', 'hello', 'slink'])

        res = self.watchmanCommand('query', root, {
            'expression': ['type', 'f'],
            'fields': ['name']})
        self.assertFileListsEqual(res['files'],
                                  ['adir/file', 'bdir/noexec.sh', 'bdir/test.sh',
                                   'hello'])

        res = self.watchmanCommand('query', root, {
            'expression': ['type', 'l'],
            'fields': ['name']})
        self.assertFileListsEqual(res['files'], ['slink'])

        res = self.watchmanCommand('query', root, {
            'expression': ['type', 'f'],
            'relative_root': 'bdir',
            'fields': ['name']})
        self.assertFileListsEqual(res['files'],
                                  ['noexec.sh', 'test.sh'])
