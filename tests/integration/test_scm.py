# vim:ts=4:sw=4:et:
# Copyright 2017-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import WatchmanTestCase
import pywatchman
import os
import subprocess


@WatchmanTestCase.expand_matrix
class TestScm(WatchmanTestCase.WatchmanTestCase):
    def hg(self, args=None, cwd=None):
        env = dict(os.environ)
        env['HGPLAIN'] = '1'
        p = subprocess.Popen(
            ['hg'] + args,
            env=env,
            cwd=cwd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE)
        out, err = p.communicate()
        if p.returncode != 0:
            raise Exception("hg %r failed: %s, %s" % (args, out, err))
        return out, err

    def test_scmHg(self):
        root = self.mkdtemp()
        ''' Set up a repo with a DAG like this:
@  changeset:   4:6c38b3c78a62
|  bookmark:    feature2
|  tag:         tip
|  summary:     add m2
|
o  changeset:   3:88fea8704cd2
|  bookmark:    TheMaster
|  parent:      1:6b3ecb11785e
|  summary:     add m1
|
| o  changeset:   2:2db357583971
|/   bookmark:    feature1
|    summary:     add f1
|
o  changeset:   0:b08db10380dd
   bookmark:    initial
   summary:     initial
        '''

        self.hg(['init'], cwd=root)
        self.touchRelative(root, 'foo')
        self.hg(['book', 'initial'], cwd=root)
        self.hg(['addremove'], cwd=root)
        self.hg(['commit', '-m', 'initial'], cwd=root)
        # Some environments prohibit locally creating "master",
        # so we use an alternative similar name.
        self.hg(['book', 'TheMaster'], cwd=root)
        self.touchRelative(root, 'bar')
        self.hg(['addremove'], cwd=root)
        self.hg(['commit', '-m', 'add bar'], cwd=root)
        self.hg(['book', 'feature1'], cwd=root)
        self.touchRelative(root, 'f1')
        self.hg(['addremove'], cwd=root)
        self.hg(['commit', '-m', 'add f1'], cwd=root)
        self.hg(['co', 'TheMaster'], cwd=root)
        self.touchRelative(root, 'm1')
        self.hg(['addremove'], cwd=root)
        self.hg(['commit', '-m', 'add m1'], cwd=root)
        self.hg(['book', 'feature2'], cwd=root)
        self.touchRelative(root, 'm2')
        self.hg(['addremove'], cwd=root)
        self.hg(['commit', '-m', 'add m2'], cwd=root)

        self.watchmanCommand('watch', root)

        res = self.watchmanCommand('query', root, {
            'expression': ['not', ['anyof', ['name', '.hg'], ['dirname', '.hg']]],
            'fields': ['name']})
        self.assertFileListsEqual(
            res['files'], ['foo', 'bar', 'm1', 'm2'])

        # Verify behavior with badly formed queries
        with self.assertRaises(pywatchman.WatchmanError) as ctx:
            self.watchmanCommand('query', root, {
                'expression': ['not', ['anyof', ['name', '.hg'], ['dirname', '.hg']]],
                'since': {'scm': {}}})
        self.assertIn(
            "key 'mergebase-with' is not present in this json object",
            str(ctx.exception))

        # When the client doesn't know the merge base, we should give
        # them the current status and merge base
        res = self.watchmanCommand('query', root, {
            'expression': ['not', ['anyof', ['name', '.hg'], ['dirname', '.hg']]],
            'fields': ['name'],
            'since': {
                'scm': {
                    'mergebase-with': 'TheMaster'}}})

        self.assertNotEqual(res['clock']['scm']['mergebase'], '')
        self.assertEqual(res['clock']['scm']['mergebase-with'], 'TheMaster')
        # The only file changed between TheMaster and feature2 is m2
        self.assertFileListsEqual(res['files'], ['m2'])

        mergeBase = res['clock']['scm']['mergebase']

        # Ensure that we can see a file that isn't tracked show up
        # as a delta in the what we consider to be the common case.
        # we're threading the merge-base result from the prior query
        # through, so this should just end up looking like a normal
        # since query.
        self.touchRelative(root, 'w00t')
        res = self.watchmanCommand('query', root, {
            'expression': ['not', ['anyof', ['name', '.hg'], ['dirname', '.hg']]],
            'fields': ['name'],
            'since': res['clock']})
        self.assertEqual(res['clock']['scm']['mergebase'], mergeBase)
        self.assertFileListsEqual(res['files'], ['w00t'])

        # Going back to the merge base, we should get a regular looking incremental
        # list of the files as we would from a since query; we expect to see
        # the removal of w00t and m2
        os.unlink(os.path.join(root, 'w00t'))
        self.hg(['co', '-C', 'TheMaster'], cwd=root)
        res = self.watchmanCommand('query', root, {
            'expression': ['not', ['anyof', ['name', '.hg'], ['dirname', '.hg']]],
            'fields': ['name'],
            'since': res['clock']})
        self.assertEqual(res['clock']['scm']['mergebase'], mergeBase)
        self.assertFileListsEqual(res['files'], ['w00t', 'm2'])

        # Now we're going to move to another branch with a different mergebase.
        self.hg(['co', '-C', 'feature1'], cwd=root)
        res = self.watchmanCommand('query', root, {
            'expression': ['not', ['anyof', ['name', '.hg'], ['dirname', '.hg']]],
            'fields': ['name'],
            'since': res['clock']})

        # We expect to observe the changed merged base
        self.assertNotEqual(res['clock']['scm']['mergebase'], mergeBase)
        # and only the file that changed since that new mergebase
        self.assertFileListsEqual(res['files'], ['f1'])
