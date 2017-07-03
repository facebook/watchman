# vim:ts=4:sw=4:et:
# Copyright 2012-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import WatchmanTestCase
import json
import os
import os.path
import sys
import re

WATCHMAN_SRC_DIR = os.environ.get('WATCHMAN_SRC_DIR', os.getcwd())
THIS_DIR = os.path.join(WATCHMAN_SRC_DIR, 'tests', 'integration')


@WatchmanTestCase.expand_matrix
class TestTrigger(WatchmanTestCase.WatchmanTestCase):

    def requiresPersistentSession(self):
        # cli transport has no log subscriptions
        return True

    def matchTriggerInLog(self, logs, root, triggerName):
        r = re.compile('%s.*posix_spawnp: %s' %
                       (re.escape(root), triggerName), re.I)
        for line in logs:
            if r.search(line.replace('\\', '/')):
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
        self.assertEqual(res['triggerid'], 'first')

        res = self.watchmanCommand('trigger', root, 'second', '*.js', '--',
                                   sys.executable, touch, second_log)
        self.assertEqual(res['triggerid'], 'second')

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

    def validate_trigger_output(self, root, files, context):
        trigger_log = os.path.join(root, 'trigger.log')
        trigger_json = os.path.join(root, 'trigger.json')

        def files_are_listed():
            if not os.path.exists(trigger_log):
                return False
            with open(trigger_log) as f:
                n = 0
                for line in f:
                    for filename in files:
                        if filename in line:
                            n = n + 1
                return n == len(files)

        self.assertWaitFor(lambda: files_are_listed(),
                           message='%s should contain %s' % (trigger_log,
                                                             json.dumps(files)))

        def files_are_listed_json():
            if not os.path.exists(trigger_json):
                return False
            expect = {}
            for f in files:
                expect[f] = True
            with open(trigger_json) as f:
                result = {}
                for line in f:
                    data = json.loads(line)
                    for item in data:
                        result[item['name']] = item['exists']

                return result == expect

        self.assertWaitFor(lambda: files_are_listed_json(),
                           message='%s should contain %s' % (trigger_json,
                                                             json.dumps(files)))

    def test_legacyTrigger(self):
        root = self.mkdtemp()

        self.touchRelative(root, 'foo.c')
        self.touchRelative(root, 'b ar.c')
        self.touchRelative(root, 'bar.txt')

        with open(os.path.join(root, '.watchmanconfig'), 'w') as f:
            json.dump({'settle': 200}, f)

        watch = self.watchmanCommand('watch', root)

        self.assertFileList(root, ['.watchmanconfig', 'b ar.c', 'bar.txt', 'foo.c'])

        res = self.watchmanCommand('trigger', root, 'test', '*.c', '--',
                                   sys.executable,
                                   os.path.join(THIS_DIR, 'trig.py'),
                                   os.path.join(root, 'trigger.log'))
        self.assertEqual('created', res['disposition'])

        res = self.watchmanCommand('trigger', root, 'other', '*.c', '--',
                                   sys.executable,
                                   os.path.join(THIS_DIR, 'trigjson.py'),
                                   os.path.join(root, 'trigger.json'))
        self.assertEqual('created', res['disposition'])

        # check that the legacy parser produced the right trigger def
        expect = [
            {'name': 'other',
             'append_files': True,
             'command': [sys.executable, os.path.join(THIS_DIR, 'trigjson.py'),
                         os.path.join(root, 'trigger.json')],
             'expression': ['anyof', ['match', '*.c', 'wholename']],
             'stdin': ['name', 'exists', 'new', 'size', 'mode']},
            {'name': 'test',
             'append_files': True,
             'command': [sys.executable, os.path.join(THIS_DIR, 'trig.py'),
                         os.path.join(root, 'trigger.log')],
             'expression': ['anyof', ['match', '*.c', 'wholename']],
             'stdin': ['name', 'exists', 'new', 'size', 'mode']}]

        triggers = self.watchmanCommand('trigger-list', root).get('triggers')
        self.assertItemsEqual(triggers, expect)

        # start collecting logs
        self.watchmanCommand('log-level', 'debug')

        self.suspendWatchman()
        self.touchRelative(root, 'foo.c')
        self.touchRelative(root, 'b ar.c')
        self.resumeWatchman()

        self.assertWaitFor(lambda: self.hasTriggerInLogs(root, 'test') and
                           self.hasTriggerInLogs(root, 'other'),
                           message='both triggers fired on update')

        self.watchmanCommand('log-level', 'off')

        self.validate_trigger_output(root, ['foo.c', 'b ar.c'], 'initial')

        def remove_logs():
            os.unlink(os.path.join(root, 'trigger.log'))
            os.unlink(os.path.join(root, 'trigger.json'))

        for f in ('foo.c', 'b ar.c'):
            # Validate that we observe the updates correctly
            # (that we're handling the since portion of the query)
            self.suspendWatchman()
            remove_logs()
            self.touchRelative(root, f)
            self.resumeWatchman()

            self.validate_trigger_output(root, [f], 'only %s' % f)

        remove_logs()

        self.watchmanCommand('log-level', 'debug')

        self.watchmanCommand('debug-recrawl', root)
        # ensure that the triggers don't get deleted
        triggers = self.watchmanCommand('trigger-list', root).get('triggers')
        self.assertItemsEqual(triggers, expect)

        self.watchmanCommand('log-level', 'off')

        self.touchRelative(root, 'foo.c')
        expect = ['foo.c']
        if watch['watcher'] == 'win32':
            # We end up re-scanning the root here and noticing that
            # b ar.c has changed.  What we're testing here is that
            # the trigger is run again, and it is ok if it notifies
            # about more files on win32, so just adjust expec:tations
            # here in the test to accomodate that difference.
            expect = ['foo.c', 'b ar.c']

        self.validate_trigger_output(root, expect, 'after recrawl')

        # Now test to see how we deal with updating the defs
        res = self.watchmanCommand('trigger', root, 'other', '*.c', '--', 'true')
        self.assertEqual('replaced', res['disposition'])

        res = self.watchmanCommand('trigger', root, 'other', '*.c', '--', 'true')
        self.assertEqual('already_defined', res['disposition'])

        # and deletion
        res = self.watchmanCommand('trigger-del', root, 'test')
        self.assertTrue(res['deleted'])
        self.assertEqual('test', res['trigger'])

        triggers = self.watchmanCommand('trigger-list', root)
        self.assertEqual(1, len(triggers['triggers']))

        res = self.watchmanCommand('trigger-del', root, 'other')
        self.assertTrue(res['deleted'])
        self.assertEqual('other', res['trigger'])

        triggers = self.watchmanCommand('trigger-list', root)
        self.assertEqual(0, len(triggers['triggers']))
