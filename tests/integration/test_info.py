# vim:ts=4:sw=4:et:
# Copyright 2012-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

# no unicode literals
from __future__ import absolute_import, division, print_function

import json
import os

import WatchmanInstance
import WatchmanTestCase


@WatchmanTestCase.expand_matrix
class TestInfo(WatchmanTestCase.WatchmanTestCase):

    def test_sock_name(self):
        resp = self.watchmanCommand("get-sockname")
        self.assertEqual(
            resp["sockname"], WatchmanInstance.getSharedInstance().getSockPath()
        )

    def test_get_config_empty(self):
        root = self.mkdtemp()
        self.watchmanCommand("watch", root)
        resp = self.watchmanCommand("get-config", root)
        self.assertEqual(resp["config"], {})

    def test_get_config(self):
        config = {"test-key": "test-value"}
        root = self.mkdtemp()
        with open(os.path.join(root, ".watchmanconfig"), "w") as f:
            json.dump(config, f)
        self.watchmanCommand("watch", root)
        resp = self.watchmanCommand("get-config", root)
        self.assertEqual(resp["config"], config)
