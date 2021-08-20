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
