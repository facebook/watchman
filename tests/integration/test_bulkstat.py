# Copyright 2014-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function

import WatchmanInstance
import WatchmanTestCase


@WatchmanTestCase.expand_matrix
class TestBulkStat(WatchmanTestCase.WatchmanTestCase):
    def test_bulkstat_on(self):
        config = {
            '_use_bulkstat': True
        }
        inst = WatchmanInstance.Instance(config=config)
        try:
            inst.start()
            client = self.getClient(inst, replace_cached=True)

            root = self.mkdtemp()
            self.client.query('watch', root)

            self.touchRelative(root, 'foo')
            self.touchRelative(root, 'bar')
            self.assertFileList(root, ['foo', 'bar'])

        finally:
            inst.stop()

    def test_bulkstat_off(self):
        config = {
            '_use_bulkstat': False
        }
        inst = WatchmanInstance.Instance(config=config)
        try:
            inst.start()
            client = self.getClient(inst, replace_cached=True)

            root = self.mkdtemp()
            self.client.query('watch', root)

            self.touchRelative(root, 'foo')
            self.touchRelative(root, 'bar')
            self.assertFileList(root, ['foo', 'bar'])

        finally:
            inst.stop()
