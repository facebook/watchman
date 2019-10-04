# vim:ts=4:sw=4:et:
# Copyright 2012-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

# no unicode literals
from __future__ import absolute_import, division, print_function

import binascii
import json
import os
import os.path
import subprocess
import unittest

import WatchmanInstance
from pywatchman import bser, compat, encoding


class TestDashJCliOption(unittest.TestCase):
    def getSockPath(self):
        return WatchmanInstance.getSharedInstance().getSockPath()

    def doJson(self, addNewLine, pretty=False):
        sockname = self.getSockPath()
        if pretty:
            watchman_cmd = b'[\n"get-sockname"\n]'
        else:
            watchman_cmd = json.dumps(["get-sockname"])
            if compat.PYTHON3:
                watchman_cmd = watchman_cmd.encode("ascii")
        if addNewLine:
            watchman_cmd = watchman_cmd + b"\n"

        cli_cmd = [
            os.environ.get("WATCHMAN_BINARY", "watchman"),
            "--sockname={0}".format(sockname),
            "--logfile=/BOGUS",
            "--statefile=/BOGUS",
            "--no-spawn",
            "--no-local",
            "-j",
        ]
        proc = subprocess.Popen(
            cli_cmd,
            stdin=subprocess.PIPE,
            stderr=subprocess.PIPE,
            stdout=subprocess.PIPE,
        )

        stdout, stderr = proc.communicate(input=watchman_cmd)
        self.assertEqual(proc.poll(), 0, stderr)
        # the response should be json because that is the default
        result = json.loads(stdout.decode("utf-8"))
        self.assertEqual(result["sockname"], sockname)

    def test_jsonInputNoNewLine(self):
        self.doJson(False)

    def test_jsonInputNewLine(self):
        self.doJson(True)

    def test_jsonInputPretty(self):
        self.doJson(True, True)

    def test_bserInput(self):
        sockname = self.getSockPath()
        watchman_cmd = bser.dumps(["get-sockname"])
        cli_cmd = [
            os.environ.get("WATCHMAN_BINARY", "watchman"),
            "--sockname={0}".format(sockname),
            "--logfile=/BOGUS",
            "--statefile=/BOGUS",
            "--no-spawn",
            "--no-local",
            "-j",
        ]
        proc = subprocess.Popen(
            cli_cmd,
            stdin=subprocess.PIPE,
            stderr=subprocess.PIPE,
            stdout=subprocess.PIPE,
        )

        stdout, stderr = proc.communicate(input=watchman_cmd)
        self.assertEqual(proc.poll(), 0, stderr)
        # the response should be bser to match our input
        result = bser.loads(stdout)
        result_sockname = result["sockname"]
        if compat.PYTHON3:
            result_sockname = encoding.decode_local(result_sockname)
        self.assertEqual(
            result_sockname, sockname, binascii.hexlify(stdout).decode("ascii")
        )
