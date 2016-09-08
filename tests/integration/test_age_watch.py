# vim:ts=4:sw=4:et:
# Copyright 2012-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import WatchmanTestCase
import tempfile
import os
import os.path
import time
import json


@WatchmanTestCase.expand_matrix
class TestAgeOutWatch(WatchmanTestCase.WatchmanTestCase):

    def makeRootAndConfig(self):
        root = self.mkdtemp()
        with open(os.path.join(root, '.watchmanconfig'), 'w') as f:
            f.write(json.dumps({
                'idle_reap_age_seconds': 1
            }))
        return root

    def listContains(self, superset, subset):
        superset = self.normWatchmanFileList(superset)
        for x in self.normFileList(subset):
            if x not in superset:
                return False
        return True

    def listNotContains(self, superset, subset):
        superset = self.normWatchmanFileList(superset)
        for x in self.normFileList(subset):
            if x in superset:
                return False
        return True

    def assertListNotContains(self, superset, subset):
        if self.listNotContains(superset, subset):
            return
        self.assertTrue(
            False, "superset: %s should not contain any of the elements of %s" % (superset, subset))

    def test_watchReap(self):
        root = self.makeRootAndConfig()
        self.watchmanCommand('watch', root)

        # make sure that we don't reap when there are registered triggers
        self.watchmanCommand('trigger', root, {
            'name': 't',
            'command': ['true']})

        # wait long enough for the reap to be considered
        time.sleep(2)

        watch_list = self.watchmanCommand('watch-list')
        self.assertTrue(self.listContains(watch_list['roots'], [root]))

        self.watchmanCommand('trigger-del', root, 't')

        # Make sure that we don't reap while we hold a subscription
        res = self.watchmanCommand('subscribe', root, 's', {
            'fields': ['name']})

        if self.transport == 'cli':
            # subscription won't stick in cli mode
            expected = []
        else:
            expected = self.normFileList([root])

        self.waitFor(lambda: self.listContains(
            self.watchmanCommand('watch-list')['roots'], expected))

        watch_list = self.watchmanCommand('watch-list')
        self.assertTrue(self.listContains(watch_list['roots'], expected))

        if self.transport != 'cli':
            # let's verify that we can safely reap two roots at once without
            # causing a deadlock
            second = self.makeRootAndConfig()
            self.watchmanCommand('watch', second)
            self.assertFileList(second, ['.watchmanconfig'])

            # and unsubscribe from root and allow it to be reaped
            unsub = self.watchmanCommand('unsubscribe', root, 's')
            self.assertTrue(unsub['deleted'], 'deleted subscription %s' % unsub)
            expected.append(self.normPath(second))

        # and now we should be ready to reap
        self.waitFor(lambda: self.listNotContains(
            self.watchmanCommand('watch-list')['roots'], expected))

        self.assertListNotContains(
            self.watchmanCommand('watch-list')['roots'], expected)

