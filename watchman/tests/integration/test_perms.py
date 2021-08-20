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


try:
    import unittest2 as unittest
except ImportError:
    import unittest


def is_root():
    return hasattr(os, "geteuid") and os.geteuid() == 0


@WatchmanTestCase.expand_matrix
class TestPerms(WatchmanTestCase.WatchmanTestCase):
    def checkOSApplicability(self):
        if os.name == "nt":
            self.skipTest("N/A on Windows")

    @unittest.skipIf(is_root(), "N/A if root")
    def test_permDeniedSubDir(self):
        root = self.mkdtemp()
        subdir = os.path.join(root, "subdir")
        os.mkdir(subdir)
        os.chmod(subdir, 0)
        self.watchmanCommand("watch", root)
        res = self.watchmanCommand(
            "query", root, {"expression": ["exists"], "fields": ["name"]}
        )
        self.assertRegex(res["warning"], "Marking this portion of the tree deleted")

    @unittest.skipIf(is_root(), "N/A if root")
    def test_permDeniedRoot(self):
        root = self.mkdtemp()
        os.chmod(root, 0)
        with self.assertRaisesRegex(pywatchman.CommandError, "(open|opendir|realpath)"):
            self.watchmanCommand("watch", root)
