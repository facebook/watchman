# vim:ts=4:sw=4:et:
# Copyright 2012-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0
import WatchmanTestCase
import tempfile
import os
import os.path
import time
import json


class TestAgeOutWatch(WatchmanTestCase.WatchmanTestCase):

    def test_watchReap(self):
        root = tempfile.mkdtemp()
        with open(os.path.join(root, '.watchmanconfig'), 'w') as f:
            f.write(json.dumps({
                'idle_reap_age_seconds': 1
            }))
        self.watchmanCommand('watch', root)

        # make sure that we don't reap when there are registered triggers
        self.watchmanCommand('trigger', root, {
            'name': 't',
            'command': ['true']})

        # wait long enough for the reap to be considered
        time.sleep(2)

        watch_list = self.watchmanCommand('watch-list')
        self.assertEqual(watch_list['roots'], [root])

        self.watchmanCommand('trigger-del', root, 't')

        # Make sure that we don't reap while we hold a subscription
        res = self.watchmanCommand('subscribe', root, 's', {
            'fields': ['name']})

        # Wait long enough for the reap to be considered
        time.sleep(2)

        watch_list = self.watchmanCommand('watch-list')
        if self.transport == 'cli':
            # subscription won't stick in cli mode
            self.assertEqual(watch_list['roots'], [])
        else:
            self.assertEqual(watch_list['roots'], [root])
            self.watchmanCommand('unsubscribe', root, 's')

        # and now we should be ready to reap
        self.waitFor(lambda: len(
            self.watchmanCommand('watch-list')['roots']) == 0)

        self.assertEqual(
            self.watchmanCommand('watch-list')['roots'], [])
