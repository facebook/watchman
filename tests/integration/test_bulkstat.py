# Copyright 2014-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

# no unicode literals
from __future__ import absolute_import, division, print_function

import WatchmanInstance
import WatchmanTestCase


@WatchmanTestCase.expand_matrix
class TestBulkStat(WatchmanTestCase.WatchmanTestCase):
    def test_bulkstat_on(self):
        config = {"_use_bulkstat": True}
        with WatchmanInstance.Instance(config=config) as inst:
            inst.start()
            self.getClient(inst, replace_cached=True)

            root = self.mkdtemp()
            self.client.query("watch", root)

            self.touchRelative(root, "foo")
            self.touchRelative(root, "bar")
            self.assertFileList(root, ["foo", "bar"])

    def test_bulkstat_off(self):
        config = {"_use_bulkstat": False}
        with WatchmanInstance.Instance(config=config) as inst:
            inst.start()
            self.getClient(inst, replace_cached=True)

            root = self.mkdtemp()
            self.client.query("watch", root)

            self.touchRelative(root, "foo")
            self.touchRelative(root, "bar")
            self.assertFileList(root, ["foo", "bar"])
