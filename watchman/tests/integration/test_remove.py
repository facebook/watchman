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
import time

import WatchmanTestCase


@WatchmanTestCase.expand_matrix
class TestRemove(WatchmanTestCase.WatchmanTestCase):
    def test_remove(self):
        root = self.mkdtemp()
        os.makedirs(os.path.join(root, "one", "two"))
        self.touchRelative(root, "one", "onefile")
        self.touchRelative(root, "one", "two", "twofile")
        self.touchRelative(root, "top")

        self.watchmanCommand("watch", root)
        self.assertFileList(
            root, files=["one", "one/onefile", "one/two", "one/two/twofile", "top"]
        )

        shutil.rmtree(os.path.join(root, "one"))

        self.assertFileList(root, files=["top"])

        self.touchRelative(root, "one")
        self.assertFileList(root, files=["top", "one"])

        self.removeRelative(root, "one")
        self.assertFileList(root, files=["top"])

        shutil.rmtree(root)
        os.makedirs(os.path.join(root, "notme"))

        self.assertWaitFor(
            lambda: not self.rootIsWatched(root),
            message="%s should be cancelled" % root,
        )
