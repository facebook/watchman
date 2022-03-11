# vim:ts=4:sw=4:et:
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.


import os

import pywatchman
from watchman.integration.lib import WatchmanTestCase


@WatchmanTestCase.expand_matrix
class TestBig(WatchmanTestCase.WatchmanTestCase):
    def checkOSApplicability(self) -> None:
        if os.name == "nt":
            self.skipTest("Windows has problems with this test")

    def test_bigQuery(self) -> None:
        root = self.mkdtemp()

        self.watchmanCommand("watch", root)

        # Create a huge query.  We're shooting for more than 2MB;
        # the server buffer size is 1MB and we want to make sure
        # we need more than 2 chunks, and we want to tickle some
        # buffer boundary conditions

        base = 2 * 1024 * 1024

        for size in range(base - 256, base + 2048, 63):
            try:
                res = self.watchmanCommand(
                    "query", root, {"expression": ["name", "a" * size]}
                )

                self.assertEqual([], res["files"])
            except pywatchman.WatchmanError as e:
                # We don't want to print the real command, as
                # it is too long, instead, replace it with
                # a summary of the size that we picked
                e.cmd = "big query with size %d" % size

                # pyre-fixme[16]: `TestBig` has no attribute `transport`.
                if self.transport == "cli":
                    e.cmd = "%s\n%s" % (e.cmd, self.getLogSample())
                raise
