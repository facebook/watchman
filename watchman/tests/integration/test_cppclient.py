# vim:ts=4:sw=4:et:
# Copyright 2016-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

# no unicode literals
from __future__ import absolute_import, division, print_function

import os
import os.path
import signal
import subprocess

import Interrupt
import WatchmanInstance


try:
    import unittest2 as unittest
except ImportError:
    import unittest

WATCHMAN_SRC_DIR = os.environ.get("WATCHMAN_SRC_DIR", os.getcwd())
TEST_BINARY = (
    os.environ["WATCHMAN_CPPCLIENT_BINARY"]
    if "WATCHMAN_CPPCLIENT_BINARY" in os.environ.keys()
    else os.path.join(WATCHMAN_SRC_DIR, "tests/integration/cppclient.t")
)


class TestCppClient(unittest.TestCase):
    @unittest.skipIf(not os.path.isfile(TEST_BINARY), "test binary not built")
    def test_cppclient(self):
        env = os.environ.copy()
        env["WATCHMAN_SOCK"] = (
            WatchmanInstance.getSharedInstance().getSockPath().legacy_sockpath()
        )
        proc = subprocess.Popen(
            TEST_BINARY, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE
        )
        (stdout, stderr) = proc.communicate()
        status = proc.poll()

        if status == -signal.SIGINT:
            Interrupt.setInterrupted()
            self.fail("Interrupted by SIGINT")
            return

        if status != 0:
            self.fail(
                "Exit status %d\n%s\n%s\n"
                % (status, stdout.decode("utf-8"), stderr.decode("utf-8"))
            )
            return

        self.assertTrue(True, TEST_BINARY)
