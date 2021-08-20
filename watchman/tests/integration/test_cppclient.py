# vim:ts=4:sw=4:et:
# Copyright (c) Facebook, Inc. and its affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import os
import os.path
import signal
import subprocess
import tempfile
import unittest

import Interrupt
import WatchmanInstance

WATCHMAN_SRC_DIR = os.environ.get("WATCHMAN_SRC_DIR", os.getcwd())
TEST_BINARY = (
    os.environ["WATCHMAN_CPPCLIENT_BINARY"]
    if "WATCHMAN_CPPCLIENT_BINARY" in os.environ.keys()
    else os.path.join(WATCHMAN_SRC_DIR, "tests/integration/cppclient.t")
)


class TestCppClient(unittest.TestCase):
    def setUp(self):
        self.tmpDirCtx = tempfile.TemporaryDirectory()  # noqa P201
        self.tmpDir = self.tmpDirCtx.__enter__()

    def tearDown(self):
        self.tmpDirCtx.__exit__(None, None, None)

    @unittest.skipIf(not os.path.isfile(TEST_BINARY), "test binary not built")
    def test_cppclient(self):
        env = os.environ.copy()
        env["WATCHMAN_SOCK"] = (
            WatchmanInstance.getSharedInstance().getSockPath().legacy_sockpath()
        )
        proc = subprocess.Popen(
            TEST_BINARY,
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            cwd=self.tmpDir,
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
