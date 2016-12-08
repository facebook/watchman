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

    def test_watchReap(self):
        root = self.makeRootAndConfig()
        self.watchmanCommand('watch', root)

        # make sure that we don't reap when there are registered triggers
        self.watchmanCommand('trigger', root, {
            'name': 't',
            'command': ['true']})

        # wait long enough for the reap to be considered
        time.sleep(2)

        self.assertTrue(self.rootIsWatched(root))

        self.watchmanCommand('trigger-del', root, 't')

        # Make sure that we don't reap while we hold a subscription
        self.watchmanCommand('subscribe', root, 's', {'fields': ['name']})

        # subscription won't stick in cli mode
        if self.transport != 'cli':
            self.assertWaitFor(lambda: self.rootIsWatched(root))

            # let's verify that we can safely reap two roots at once without
            # causing a deadlock
            second = self.makeRootAndConfig()
            self.watchmanCommand('watch', second)
            self.assertFileList(second, ['.watchmanconfig'])

            # and unsubscribe from root and allow it to be reaped
            unsub = self.watchmanCommand('unsubscribe', root, 's')
            self.assertTrue(unsub['deleted'], 'deleted subscription %s' % unsub)
            # and now we should be ready to reap
            self.assertWaitFor(lambda: not self.rootIsWatched(
                root) and not self.rootIsWatched(second))

