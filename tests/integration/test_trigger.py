# vim:ts=4:sw=4:et:
# Copyright 2012-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import WatchmanTestCase
import os
import os.path
import sys
import re

WATCHMAN_SRC_DIR = os.environ.get('WATCHMAN_SRC_DIR', os.getcwd())
THIS_DIR = os.path.join(WATCHMAN_SRC_DIR, 'tests', 'integration')


@WatchmanTestCase.expand_matrix
class TestTriggerIssue141(WatchmanTestCase.WatchmanTestCase):

    def requiresPersistentSession(self):
        # cli transport has no log subscriptions
        return True

    def matchTriggerInLog(self, logs, root, triggerName):
        r = re.compile(' trigger %s:%s pid=' %
                       (re.escape(root), triggerName), re.I)
        for line in logs:
            if r.search(line):
                return True
        return False

    def hasTriggerInLogs(self, root, triggerName):
        client = self.getClient()
        logs = client.getLog(remove=False)
        if self.matchTriggerInLog(logs, root, triggerName):
            return True
        res = client.receive()
        while client.isUnilateralResponse(res):
            logs = client.getLog(remove=False)
            if self.matchTriggerInLog(logs, root, triggerName):
                return True
            res = client.receive()
        return False

    # https://github.com/facebook/watchman/issues/141
    def test_triggerIssue141(self):
        root = self.mkdtemp()
        self.touchRelative(root, 'foo.js')

        self.watchmanCommand('watch', root)
        self.assertFileList(root, ['foo.js'])

        touch = os.path.join(THIS_DIR, 'touch.py')
        logs = self.mkdtemp()
        first_log = os.path.join(logs, 'first')
        second_log = os.path.join(logs, 'second')

        res = self.watchmanCommand('trigger', root, 'first', '*.js', '--',
                                   sys.executable, touch, first_log)
        self.assertEqual(self.decodeBSERUTF8(res['triggerid']), 'first')

        res = self.watchmanCommand('trigger', root, 'second', '*.js', '--',
                                   sys.executable, touch, second_log)
        self.assertEqual(self.decodeBSERUTF8(res['triggerid']), 'second')

        self.assertWaitFor(lambda: os.path.exists(first_log) and
                           os.path.exists(second_log),
                           message='both triggers fire at start')

        # start collecting logs
        self.watchmanCommand('log-level', 'debug')
        # touch the file, should run both triggers
        self.touchRelative(root, 'foo.js')

        self.assertWaitFor(lambda: self.hasTriggerInLogs(root, 'first') and
                           self.hasTriggerInLogs(root, 'second'),
                           message='both triggers fired on update')
