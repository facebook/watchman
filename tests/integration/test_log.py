# vim:ts=4:sw=4:et:
# Copyright 2017-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

# no unicode literals
from __future__ import absolute_import, division, print_function

import pywatchman
import WatchmanTestCase


@WatchmanTestCase.expand_matrix
class TestLog(WatchmanTestCase.WatchmanTestCase):

    def test_invalidNumArgsLogLevel(self):
        for params in [["log-level"], ["log-level", "debug", "extra"]]:
            with self.assertRaises(pywatchman.WatchmanError) as ctx:
                self.watchmanCommand(*params)

            self.assertIn("wrong number of arguments", str(ctx.exception))

    def test_invalidLevelLogLevel(self):
        with self.assertRaises(pywatchman.WatchmanError) as ctx:
            self.watchmanCommand("log-level", "invalid")

        self.assertIn("invalid log level", str(ctx.exception))

    def test_invalidNumArgsLog(self):
        for params in [["log"], ["log", "debug"], ["log", "debug", "test", "extra"]]:
            with self.assertRaises(pywatchman.WatchmanError) as ctx:
                self.watchmanCommand(*params)

            self.assertIn("wrong number of arguments", str(ctx.exception))

    def test_invalidLevelLog(self):
        with self.assertRaises(pywatchman.WatchmanError) as ctx:
            self.watchmanCommand("log", "invalid", "test")

        self.assertIn("invalid log level", str(ctx.exception))
