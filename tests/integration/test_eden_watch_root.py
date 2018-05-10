# vim:ts=4:sw=4:et:
# Copyright 2017-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

# no unicode literals
from __future__ import absolute_import, division, print_function

import os

import pywatchman
import WatchmanEdenTestCase


class TestEdenWatchRoot(WatchmanEdenTestCase.WatchmanEdenTestCase):

    def test_eden_watch_root(self):

        def populate(repo):
            repo.write_file("adir/file", "foo!\n")
            repo.commit("initial commit.")

        root = self.makeEdenMount(populate)

        with self.assertRaises(pywatchman.WatchmanError) as ctx:
            self.watchmanCommand("watch", os.path.join(root, "adir"))
        self.assertRegex(
            str(ctx.exception),
            (
                "unable to resolve root .*: eden: you may only watch "
                + "from the root of an eden mount point."
            ),
        )
