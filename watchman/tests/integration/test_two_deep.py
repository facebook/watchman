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
import time

import WatchmanTestCase


@WatchmanTestCase.expand_matrix
class TestTwoDeep(WatchmanTestCase.WatchmanTestCase):
    def test_two_deep(self):
        root = self.mkdtemp()
        self.watchmanCommand("watch", root)

        os.makedirs(os.path.join(root, "foo", "bar"))

        # Guarantee that 111's mtime is greater than its parent dirs
        time.sleep(1)

        with open(os.path.join(root, "foo", "bar", "111"), "w") as f:
            f.write("111")

        self.assertFileList(root, files=["foo", "foo/bar", "foo/bar/111"])

        res_111 = self.watchmanCommand("find", root, "foo/bar/111")["files"][0]
        st_111 = os.lstat(os.path.join(root, "foo", "bar", "111"))

        res_bar = self.watchmanCommand("find", root, "foo/bar")["files"][0]
        st_bar = os.lstat(os.path.join(root, "foo", "bar"))

        self.assertEqual(res_111["mtime"], int(st_111.st_mtime))
        self.assertEqual(res_bar["mtime"], int(st_bar.st_mtime))
