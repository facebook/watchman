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

import WatchmanTestCase


@WatchmanTestCase.expand_matrix
class TestCaseSensitive(WatchmanTestCase.WatchmanTestCase):
    def test_changeCase(self):
        root = self.mkdtemp()
        os.mkdir(os.path.join(root, "foo"))
        self.watchmanCommand("watch", root)
        self.assertFileList(root, ["foo"])

        os.rename(os.path.join(root, "foo"), os.path.join(root, "FOO"))
        self.touchRelative(root, "FOO", "bar")
        self.assertFileList(root, ["FOO", "FOO/bar"])

        os.rename(os.path.join(root, "FOO", "bar"), os.path.join(root, "FOO", "BAR"))
        self.assertFileList(root, ["FOO", "FOO/BAR"])

        os.rename(os.path.join(root, "FOO"), os.path.join(root, "foo"))
        self.assertFileList(root, ["foo", "foo/BAR"])

        os.mkdir(os.path.join(root, "foo", "baz"))
        self.touchRelative(root, "foo", "baz", "file")
        self.assertFileList(root, ["foo", "foo/BAR", "foo/baz", "foo/baz/file"])

        os.rename(os.path.join(root, "foo"), os.path.join(root, "Foo"))

        self.assertFileList(root, ["Foo", "Foo/BAR", "Foo/baz", "Foo/baz/file"])
