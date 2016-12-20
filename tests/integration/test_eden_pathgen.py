# vim:ts=4:sw=4:et:
# Copyright 2016-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import WatchmanEdenTestCase


class TestEdenPathGenerator(WatchmanEdenTestCase.WatchmanEdenTestCase):
    def test_eden_watch(self):
        root = self.makeEdenMount()
        res = self.watchmanCommand('watch', root)
        self.assertEqual('eden', res['watcher'])
