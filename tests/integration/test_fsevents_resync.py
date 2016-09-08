# vim:ts=4:sw=4:et:
# Copyright 2016-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import WatchmanTestCase
import json
import tempfile
import os
import os.path
import sys


@WatchmanTestCase.expand_matrix
class TestFSEventsResync(WatchmanTestCase.WatchmanTestCase):
    def checkOSApplicability(self):
        if sys.platform != 'darwin':
            self.skipTest('N/A unless macOS')

    def test_resync(self):
        root = self.mkdtemp()
        with open(os.path.join(root, '.watchmanconfig'), 'w') as f:
            f.write(json.dumps({
                'fsevents_try_resync': True
            }))

        self.watchmanCommand('watch', root)
        self.touchRelative(root, '111')
        self.assertFileList(root, ['.watchmanconfig', '111'])

        res = self.watchmanCommand('query', root, {
            'fields': ['name']})
        self.assertTrue(res['is_fresh_instance'])
        clock = res['clock']

        dropinfo = self.watchmanCommand('debug-fsevents-inject-drop', root)
        self.assertTrue('last_good' in dropinfo, dropinfo)

        # We expect to see the results of these two filesystem operations
        # on our next query, and not see evidence of a recrawl
        os.unlink(os.path.join(root, '111'))
        self.touchRelative(root, '222')

        res = self.watchmanCommand('query', root, {
            'since': clock,
            'expression': ['exists'],
            'fields': ['name']})
        self.assertFalse(res['is_fresh_instance'], res)
        self.assertTrue('warning' not in res, res)
        self.assertEqual(self.normWatchmanFileList(res['files']),
                         self.normFileList(['222']))

