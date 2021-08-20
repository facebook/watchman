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
import shutil

import WatchmanTestCase


@WatchmanTestCase.expand_matrix
class TestRemoveThenAdd(WatchmanTestCase.WatchmanTestCase):
    def checkOSApplicability(self):
        if os.name == "linux" and os.getenv("TRAVIS"):
            self.skipTest("openvz and inotify unlinks == bad time")

    def test_remove_then_add(self):
        root = self.mkdtemp()
        os.mkdir(os.path.join(root, "foo"))

        self.watchmanCommand("watch", root)

        self.touchRelative(root, "foo", "222")
        os.mkdir(os.path.join(root, "foo", "bar"))

        self.assertFileList(root, files=["foo", "foo/bar", "foo/222"])

        shutil.rmtree(os.path.join(root, "foo", "bar"))
        self.removeRelative(root, "foo", "222")
        shutil.rmtree(os.path.join(root, "foo"))

        self.assertFileList(root, files=[])

        os.mkdir(os.path.join(root, "foo"))
        self.touchRelative(root, "foo", "222")

        self.assertFileList(root, files=["foo", "foo/222"])
