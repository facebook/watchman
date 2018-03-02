# vim:ts=4:sw=4:et:
# Copyright 2012-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import WatchmanInstance
import WatchmanTestCase
import json
import os


@WatchmanTestCase.expand_matrix
class TestInfo(WatchmanTestCase.WatchmanTestCase):
    def test_sock_name(self):
        resp = self.watchmanCommand('get-sockname')
        self.assertEqual(resp['sockname'],
                         WatchmanInstance.getSharedInstance().getSockPath())

    def test_get_config_empty(self):
        root = self.mkdtemp()
        self.watchmanCommand('watch', root)
        resp = self.watchmanCommand('get-config', root)
        self.assertEqual(resp['config'], {})

    def test_get_config(self):
        config = {
            'test-key': 'test-value',
        }
        root = self.mkdtemp()
        with open(os.path.join(root, '.watchmanconfig'), 'w') as f:
            json.dump(config, f)
        self.watchmanCommand('watch', root)
        resp = self.watchmanCommand('get-config', root)
        self.assertEqual(resp['config'], config)
