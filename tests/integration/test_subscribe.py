# vim:ts=4:sw=4:et:
# Copyright 2015-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import WatchmanTestCase
import tempfile
import os
import os.path
import unittest
import pywatchman


class TestSubscribe(WatchmanTestCase.WatchmanTestCase):

    def requiresPersistentSession(self):
        return True

    def wlockExists(self, subdata, exists):
        norm_wlock = self.normPath('.hg/wlock')
        for sub in subdata:
            for f in sub['files']:
                if f['exists'] == exists and \
                        self.normPath(f['name']) == norm_wlock:
                    return True
        return False

    def matchStateSubscription(self, subdata, mode):
        for sub in subdata:
            if mode in sub:
                return sub
        return None

    def test_defer_state(self):
        root = self.mkdtemp()
        self.watchmanCommand('watch', root)

        self.watchmanCommand('subscribe', root, 'defer', {
            'fields': ['name'],
            'defer': ['foo']})

        self.touchRelative(root, 'a')
        self.assertNotEqual(None, self.waitForSub('defer', root=root))

        self.watchmanCommand('state-enter', root, 'foo')
        begin = self.waitForSub('defer', root)[0]
        self.assertEqual(begin['state-enter'], 'foo')

        self.touchRelative(root, 'in-foo')
        # We expect this to timeout because state=foo is asserted
        with self.assertRaises(pywatchman.SocketTimeout):
            self.waitForSub('defer', root, timeout=1)

        self.watchmanCommand('state-leave', root, 'foo')

        self.assertNotEqual(None,
            self.waitForSub('defer', root,
            accept=lambda x: self.matchStateSubscription(x, 'state-leave')))

        # and now we should observe the file change
        self.assertNotEqual(None,
            self.waitForSub('defer', root))

        # and again, but this time passing metadata
        self.watchmanCommand('state-enter', root, {
          'name': 'foo',
          'metadata': 'meta!'})
        begin = self.waitForSub('defer', root)[0]
        self.assertEqual(begin['state-enter'], 'foo')
        self.assertEqual(begin['metadata'], 'meta!')

        self.touchRelative(root, 'in-foo-2')
        # We expect this to timeout because state=foo is asserted
        with self.assertRaises(pywatchman.SocketTimeout):
            self.waitForSub('defer', root, timeout=1)

        self.watchmanCommand('state-leave', root, {
          'name': 'foo',
          'metadata': 'leavemeta'})

        end = self.waitForSub('defer', root,
                accept=lambda x: self.matchStateSubscription(
                    x, 'state-leave'))[0]
        self.assertEqual(end['metadata'], 'leavemeta')

        # and now we should observe the file change
        self.assertNotEqual(None,
            self.waitForSub('defer', root))

    def test_drop_state(self):
        root = self.mkdtemp()
        self.watchmanCommand('watch', root)

        self.watchmanCommand('subscribe', root, 'drop', {
            'fields': ['name'],
            'drop': ['foo']})

        self.touchRelative(root, 'a')
        self.assertNotEqual(None, self.waitForSub('drop', root=root))

        self.watchmanCommand('state-enter', root, 'foo')
        begin = self.waitForSub('drop', root)[0]
        self.assertEqual(begin['state-enter'], 'foo')

        self.touchRelative(root, 'in-foo')
        # We expect this to timeout because state=foo is asserted
        with self.assertRaises(pywatchman.SocketTimeout):
            self.waitForSub('drop', root, timeout=1)

        self.watchmanCommand('state-leave', root, 'foo')

        self.assertNotEqual(None,
            self.waitForSub('drop', root,
            accept=lambda x: self.matchStateSubscription(x, 'state-leave')))

        # There should be no more subscription data to observe
        # because we requested that it be dropped
        with self.assertRaises(pywatchman.SocketTimeout):
            self.waitForSub('drop', root, timeout=1)

        # let's make sure that we can observe new changes
        self.touchRelative(root, 'out-foo')

        self.assertFileList(root, files=['a', 'in-foo', 'out-foo'])
        self.assertNotEqual(None,
            self.waitForSub('drop', root))

    def test_defer_vcs(self):
        root = self.mkdtemp()
        # fake an hg control dir
        os.mkdir(os.path.join(root, '.hg'))
        self.watchmanCommand('watch', root)
        self.assertFileList(root, files=['.hg'])

        sub = self.watchmanCommand('subscribe', root, 'defer', {
            'fields': ['name', 'exists'],
            'defer_vcs': True})

        dat = self.waitForSub('defer', root)[0]
        self.assertEqual(True, dat['is_fresh_instance'])
        self.assertEqual([{'name': '.hg', 'exists': True}], dat['files'])

        # Pretend that hg is update the working copy
        self.touchRelative(root, '.hg', 'wlock')

        # We expect this to timeout because the wlock file exists
        with self.assertRaises(pywatchman.SocketTimeout):
            self.waitForSub('defer', root,
                                  accept=lambda x: self.wlockExists(x, True),
                                  timeout=2)

        # Remove the wlock and allow subscriptions to flow
        os.unlink(os.path.join(root, '.hg', 'wlock'))

        # The events should get coalesced to a delete for wlock
        dat = self.waitForSub('defer', root,
                              timeout=2,
                              accept=lambda x: self.wlockExists(x, False))
        self.assertNotEqual(None, dat)

    def test_immediate_subscribe(self):
        root = self.mkdtemp()
        # fake an hg control dir
        os.mkdir(os.path.join(root, '.hg'))
        self.watchmanCommand('watch', root)
        self.assertFileList(root, files=['.hg'])

        sub = self.watchmanCommand('subscribe', root, 'nodefer', {
            'fields': ['name', 'exists'],
            'defer_vcs': False})

        dat = self.waitForSub('nodefer', root)[0]
        self.assertEqual(True, dat['is_fresh_instance'])
        self.assertEqual([{'name': '.hg', 'exists': True}], dat['files'])

        # Pretend that hg is update the working copy
        self.touchRelative(root, '.hg', 'wlock')

        dat = self.waitForSub('nodefer', root,
                              accept=lambda x: self.wlockExists(x, True))
        # We observed the changes even though wlock existed
        self.assertNotEqual(None, dat)

        os.unlink(os.path.join(root, '.hg', 'wlock'))

        dat = self.waitForSub('nodefer', root,
                              accept=lambda x: self.wlockExists(x, False))
        self.assertNotEqual(None, dat)

    def test_subscribe(self):
        root = self.mkdtemp()
        a_dir = os.path.join(root, 'a')
        os.mkdir(a_dir)
        self.touchRelative(a_dir, 'lemon')
        self.touchRelative(root, 'b')

        self.watchmanCommand('watch', root)
        self.assertFileList(root, files=['a', 'a/lemon', 'b'])

        sub = self.watchmanCommand('subscribe', root, 'myname', {
            'fields': ['name']})

        rel_sub = self.watchmanCommand('subscribe', root, 'relative', {
            'fields': ['name'],
            'relative_root': 'a'})

        # prove initial results come through
        dat = self.waitForSub('myname', root=root)[0]
        self.assertEqual(True, dat['is_fresh_instance'])
        self.assertEqual(self.normWatchmanFileList(dat['files']),
                         self.normFileList(['a', 'a/lemon', 'b']))

        # and that relative_root adapts the path name
        dat = self.waitForSub('relative', root=root)[0]
        self.assertEqual(True, dat['is_fresh_instance'])
        self.assertEqual(self.normWatchmanFileList(dat['files']),
                         self.normFileList(['lemon']))

        # check that deletes show up in the subscription results
        os.unlink(os.path.join(root, 'a', 'lemon'))
        dat = self.waitForSub('myname', root=root,
                              accept=lambda x: self.findSubscriptionContainingFile(x, 'a/lemon'))
        self.assertNotEqual(None, dat)
        self.assertEqual(False, dat[0]['is_fresh_instance'])

        dat = self.waitForSub('relative', root=root,
                              accept=lambda x: self.findSubscriptionContainingFile(x, 'lemon'))
        self.assertNotEqual(None, dat)
        self.assertEqual(False, dat[0]['is_fresh_instance'])

        # Trigger a recrawl and ensure that the subscription isn't lost
        self.watchmanCommand('debug-recrawl', root)

        ab = self.normFileList(['a', 'b'])

        def matchesRecrawledDir(subdata):
            for sub in subdata:
                if not sub['is_fresh_instance']:
                    continue
                files = self.normFileList(sub['files'])
                if files == ab:
                    return True
            return False

        dat = self.waitForSub('myname', root=root,
                              accept=matchesRecrawledDir)
        self.assertNotEqual(None, dat)

        # Ensure that we observed the recrawl warning
        warn = None
        for item in dat:
            if 'warning' in item:
                warn = item['warning']
                break
        self.assertRegexpMatches(warn, r'Recrawled this watch')
