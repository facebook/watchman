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

import json
import os

import pywatchman
import WatchmanInstance


try:
    import unittest2 as unittest
except ImportError:
    import unittest


WATCHMAN_SRC_DIR = os.environ.get("WATCHMAN_SRC_DIR", os.getcwd())
THIS_DIR = os.path.join(WATCHMAN_SRC_DIR, "tests", "integration")


@unittest.skipIf(os.name == "nt", "not supported on windows")
class TestSiteSpawn(unittest.TestCase):
    if not pywatchman.compat.PYTHON3:
        assertRegex = unittest.TestCase.assertRegexpMatches

    def test_failingSpawner(self):
        config = {
            "spawn_watchman_service": os.path.join(THIS_DIR, "site_spawn_fail.py")
        }

        inst = WatchmanInstance.Instance(config=config)
        stdout, stderr = inst.commandViaCLI(["version"])
        print("stdout", stdout)
        print("stderr", stderr)
        stderr = stderr.decode("ascii")
        self.assertEqual(b"", stdout)
        self.assertRegex(stderr, "failed to start\n")
        self.assertRegex(stderr, "site_spawn_fail.py: exited with status 1")

    def test_no_site_spawner(self):
        """With a site spawner configured to otherwise fail, pass
        `--no-site-spawner` and ensure that a failure didn't occur."""
        config = {
            "spawn_watchman_service": os.path.join(THIS_DIR, "site_spawn_fail.py")
        }

        inst = WatchmanInstance.Instance(config=config)
        stdout, stderr = inst.commandViaCLI(["version", "--no-site-spawner"])

        print(stdout, stderr.decode("ascii"))
        parsed = json.loads(stdout.decode("ascii"))
        self.assertTrue("version" in parsed)

        inst.commandViaCLI(["--no-spawn", "--no-local", "shutdown-server"])

    def test_spawner(self):
        config = {"spawn_watchman_service": os.path.join(THIS_DIR, "site_spawn.py")}

        inst = WatchmanInstance.Instance(config=config)
        stdout, stderr = inst.commandViaCLI(["version"])

        parsed = json.loads(stdout.decode("ascii"))
        self.assertTrue("version" in parsed)

        # Shut down that process, as we have no automatic way to deal with it
        inst.commandViaCLI(["--no-spawn", "--no-local", "shutdown-server"])
