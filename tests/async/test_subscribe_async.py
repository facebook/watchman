
# vim:ts=4:sw=4:et:
# Copyright 2015-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import AsyncWatchmanTestCase
import tempfile
import os
import os.path
try:
    import unittest2 as unittest
except ImportError:
    import unittest
import pywatchman


class TestSubscribe(AsyncWatchmanTestCase.AsyncWatchmanTestCase):

    def test_subscribe(self):
        root = tempfile.mkdtemp()
        a_dir = os.path.join(root, 'a')
        os.mkdir(a_dir)
        self.touch_relative(a_dir, 'lemon')
        self.touch_relative(root, 'b')

        self.watchman_command('watch', root)
        self.assert_root_file_set(root, files=['a', 'a/lemon', 'b'])

        sub = self.watchman_command('subscribe', root, 'myname', {
            'fields': ['name']})

        rel_sub = self.watchman_command('subscribe', root, 'relative', {
            'fields': ['name'],
            'relative_root': 'a'})

        # prove initial results come through
        dat = self.wait_for_sub('myname', root=root)
        self.assertEqual(True, dat['is_fresh_instance'])
        self.assert_file_sets_equal(dat['files'], ['a', 'a/lemon', 'b'])

        # and that relative_root adapts the path name
        dat = self.wait_for_sub('relative', root=root)
        self.assertEqual(True, dat['is_fresh_instance'])
        self.assert_file_sets_equal(dat['files'], ['lemon'])

        # check that deletes show up in the subscription results
        os.unlink(os.path.join(root, 'a', 'lemon'))
        dat = self.wait_for_sub('myname', root=root)
        self.assertNotEqual(None, dat)
        self.assertEqual(False, dat['is_fresh_instance'])
        self.assert_sub_contains_all(dat, ['a/lemon'])

        dat = self.wait_for_sub('relative', root=root)
        self.assertNotEqual(None, dat)
        self.assertEqual(False, dat['is_fresh_instance'])
        self.assert_sub_contains_all(dat, ['lemon'])

        # Trigger a recrawl and ensure that the subscription isn't lost
        self.watchman_command('debug-recrawl', root)
        self.assertNotEqual(None, dat)
        self.assertEqual(False, dat['is_fresh_instance'])
        dat = self.wait_for_sub('myname', root=root)
        self.assert_sub_contains_all(dat, ['a', 'b'])

        # Ensure that we observed the recrawl warning
        warn = None
        if 'warning' in dat:
            warn = dat['warning']
        self.assertRegexpMatches(warn, rb'Recrawled this watch')

