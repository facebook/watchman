# vim:ts=4:sw=4:et:
# Copyright 2016-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

# no unicode literals
from __future__ import absolute_import, division, print_function

import os
import sys

import pywatchman
import WatchmanInstance


try:
    import unittest2 as unittest
except ImportError:
    import unittest


@unittest.skipIf(os.name == "nt", "N/A on windows")
class TestNice(unittest.TestCase):
    if not pywatchman.compat.PYTHON3:
        assertRegex = unittest.TestCase.assertRegexpMatches

    def test_failing_to_start_when_nice(self):
        if sys.platform == "darwin":
            self.skipTest("launchd renders this test invalid on macOS")
        inst = WatchmanInstance.Instance()
        stdout, stderr = inst.commandViaCLI(["version"], prefix=["nice"])
        print("stdout", stdout)
        print("stderr", stderr)
        stderr = stderr.decode("ascii")
        self.assertEqual(b"", stdout)
        self.assertRegex(stderr, "refusing to start")

    def test_failing_to_start_when_nice_foreground(self):
        inst = WatchmanInstance.Instance()
        stdout, stderr = inst.commandViaCLI(
            ["--foreground", "version"], prefix=["nice"]
        )
        print("stdout", stdout)
        print("stderr", stderr)

        output = stderr.decode("ascii")
        try:
            output += inst.getServerLogContents()
        except IOError:
            # on macos, we may not have gotten as far
            # as creating the log file when the error
            # triggers, so we're fine with passing
            # on io errors here
            pass
        self.assertRegex(output, "refusing to start")
