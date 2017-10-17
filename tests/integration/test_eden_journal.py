# vim:ts=4:sw=4:et:
# Copyright 2017-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import WatchmanEdenTestCase


class TestEdenJournal(WatchmanEdenTestCase.WatchmanEdenTestCase):

    def test_eden_journal(self):
        def populate(repo):
            repo.write_file('hello', 'hola\n')
            repo.commit('initial commit.')

        root = self.makeEdenMount(populate, enable_hg=True)
        repo = self.repoForPath(root)
        initial_commit = repo.get_head_hash()

        res = self.watchmanCommand('watch', root)
        self.assertEqual('eden', res['watcher'])

        clock = self.watchmanCommand('clock', root)

        self.touchRelative(root, 'newfile')

        res = self.watchmanCommand('query', root, {
            'fields': ['name'],
            'since': clock})
        clock = res['clock']
        self.assertFileListsEqual(res['files'], ['newfile'])

        repo.add_file('newfile')
        repo.commit(message='add newfile')
        res = self.watchmanCommand('query', root, {
            'expression': ['not', ['dirname', '.hg']],
            'fields': ['name'],
            'since': clock})
        clock = res['clock']
        self.assertFileListsEqual(res['files'], [
            'newfile'],
            message='We expect to report the files changed in the commit')

        # Test the the journal has the correct contents across a "reset" like
        # operation where the parents are poked directly.   This is using
        # debugsetparents rather than reset because the latter isn't enabled
        # by default for hg in the watchman test machinery.
        self.touchRelative(root, 'unclean')
        repo.hg('debugsetparents', initial_commit)
        res = self.watchmanCommand('query', root, {
            'expression': ['not', ['dirname', '.hg']],
            'fields': ['name'],
            'since': clock})
        self.assertFileListsEqual(res['files'], [
            'newfile',
            'unclean'],
            message=('We expect to report the file changed in the commit '
                     'as well as the unclean file'))
