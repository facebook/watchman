# vim:ts=4:sw=4:et:
# Copyright 2016-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import WatchmanTestCase
import pywatchman


@WatchmanTestCase.expand_matrix
class TestRecrawl(WatchmanTestCase.WatchmanTestCase):

    def test_failDontWaitForRecrawl(self):
        root = self.mkdtemp()
        self.touchRelative(root, '111')

        self.watchmanCommand('watch', root)

        # Force the next recrawl to have a delay
        self.watchmanCommand('debug-delay-next-recrawl', root, 1000)

        # Trigger the recrawl
        self.watchmanCommand('debug-recrawl', root)

        with self.assertRaises(pywatchman.WatchmanError):
            self.watchmanCommand('query', root, {
                'relative_root': 'subdir',
                'fields': ['name'],
                "dont_wait_for_recrawl": True })
