# vim:ts=4:sw=4:et:
# Copyright 2016-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

# no unicode literals
from __future__ import absolute_import, division, print_function

import os
import os.path
import sys

import WatchmanTestCase


@WatchmanTestCase.expand_matrix
class TestKQueueAndFSEventsRecrawl(WatchmanTestCase.WatchmanTestCase):
    def checkOSApplicability(self):
        if sys.platform != "darwin":
            self.skipTest("N/A unless macOS")

    def test_recrawl(self):
        root = self.mkdtemp()
        watch = self.watchmanCommand("watch", root)

        # On macOS, we may not always use kqueue+fsevents
        if watch["watcher"] != "kqueue+fsevents":
            return

        os.mkdir(os.path.join(root, "foo"))
        filelist = ["foo"]

        self.assertFileList(root, filelist)

        self.suspendWatchman()

        filelist = ["foo"]
        for i in range(3000):
            self.touchRelative(root, "foo", str(i))
            filelist.append(f"foo/{i}")

        self.resumeWatchman()

        self.watchmanCommand(
            "debug-kqueue-and-fsevents-recrawl", root, os.path.join(root, "foo")
        )

        self.assertFileList(root, filelist)
