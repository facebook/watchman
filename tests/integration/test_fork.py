# vim:ts=4:sw=4:et:
# Copyright 2012-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

# no unicode literals
from __future__ import absolute_import, division, print_function

import os

import pywatchman
import WatchmanTestCase


@WatchmanTestCase.expand_matrix
class TestSince(WatchmanTestCase.WatchmanTestCase):
    def checkOSApplicability(self):
        if not getattr(os, "fork", None):
            self.skipTest("no fork on this system")

    def test_forkclient(self):
        client = self.getClient()

        client.query("version")

        pid = os.fork()
        if pid == 0:
            # I am the new process
            try:
                with self.assertRaises(pywatchman.UseAfterFork) as ctx:
                    client.query("version")
                self.assertIn(
                    "do not re-use a connection after fork", str(ctx.exception)
                )

                # All good
                os._exit(0)
            except BaseException as exc:
                print("Error in child process: %s" % exc)
                os._exit(1)

        _pid, status = os.waitpid(pid, 0)
        self.assertEqual(status, 0, "child process exited 0")
