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
class TestSuffixGenerator(WatchmanTestCase.WatchmanTestCase):
    def test_suffix_generator(self):
        root = self.mkdtemp()

        # Suffix queries are defined as being case insensitive.
        # Use an uppercase suffix to verify that our lowercase
        # suffixes in the pattern are matching correctly.
        self.touchRelative(root, "foo.C")
        os.mkdir(os.path.join(root, "subdir"))
        self.touchRelative(root, "subdir", "bar.txt")

        self.watchmanCommand("watch", root)

        self.assertFileListsEqual(
            self.watchmanCommand("query", root, {"suffix": "c", "fields": ["name"]})[
                "files"
            ],
            ["foo.C"],
        )

        self.assertFileListsEqual(
            self.watchmanCommand(
                "query", root, {"suffix": ["c", "txt"], "fields": ["name"]}
            )["files"],
            ["foo.C", "subdir/bar.txt"],
        )

        self.assertFileListsEqual(
            self.watchmanCommand(
                "query",
                root,
                {"suffix": ["c", "txt"], "relative_root": "subdir", "fields": ["name"]},
            )["files"],
            ["bar.txt"],
        )

        with self.assertRaises(pywatchman.WatchmanError) as ctx:
            self.watchmanCommand("query", root, {"suffix": {"a": "b"}})

        self.assertRegex(
            str(ctx.exception), "'suffix' must be a string or an array of strings"
        )

    def test_suffix_generator_empty(self):
        """Specifying no input suffixes should return no results."""
        root = self.mkdtemp()

        os.mkdir(os.path.join(root, "mydir"))
        os.mkdir(os.path.join(root, "mydir.dir"))
        self.touchRelative(root, "myfile")
        self.touchRelative(root, "myfile.txt")
        self.watchmanCommand("watch", root)

        self.assertFileListsEqual(
            self.watchmanCommand("query", root, {"fields": ["name"], "suffix": []})[
                "files"
            ],
            [],
        )
