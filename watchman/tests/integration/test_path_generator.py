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

import WatchmanTestCase


@WatchmanTestCase.expand_matrix
class TestPathGenerator(WatchmanTestCase.WatchmanTestCase):
    def test_path_generator_dot(self):
        root = self.mkdtemp()

        self.watchmanCommand("watch", root)
        self.assertFileListsEqual(
            self.watchmanCommand("query", root, {"path": ["."]})["files"], []
        )

        self.assertFileListsEqual(
            self.watchmanCommand("query", root, {"relative_root": ".", "path": ["."]})[
                "files"
            ],
            [],
        )

    def test_path_generator_case(self):
        root = self.mkdtemp()

        os.mkdir(os.path.join(root, "foo"))
        self.touchRelative(root, "foo", "bar")
        self.watchmanCommand("watch", root)

        self.assertFileListsEqual(
            self.watchmanCommand("query", root, {"fields": ["name"], "path": ["foo"]})[
                "files"
            ],
            ["foo/bar"],
        )

        if self.isCaseInsensitive():
            os.rename(os.path.join(root, "foo"), os.path.join(root, "Foo"))

            self.assertFileListsEqual(
                self.watchmanCommand(
                    "query", root, {"fields": ["name"], "path": ["foo"]}  # not Foo!
                )["files"],
                [],
                message="Case insensitive matching not implemented \
                        for path generator",
            )

    def test_path_generator_relative_root(self):
        root = self.mkdtemp()

        os.mkdir(os.path.join(root, "foo"))
        self.touchRelative(root, "foo", "bar")
        self.watchmanCommand("watch", root)

        self.assertFileListsEqual(
            self.watchmanCommand(
                "query",
                root,
                {"fields": ["name"], "relative_root": "foo", "path": ["bar"]},
            )["files"],
            ["bar"],
        )

        self.assertFileListsEqual(
            self.watchmanCommand(
                "query",
                root,
                {
                    "fields": ["name"],
                    "relative_root": "foo",
                    "path": [{"path": "bar", "depth": -1}],
                },
            )["files"],
            ["bar"],
        )

        if self.isCaseInsensitive():
            os.rename(os.path.join(root, "foo"), os.path.join(root, "Foo"))

            self.assertFileListsEqual(
                self.watchmanCommand(
                    "query", root, {"fields": ["name"], "path": ["foo"]}  # not Foo!
                )["files"],
                [],
                message="Case insensitive matching not implemented \
                        for path relative_root",
            )

    def test_path_generator_empty(self):
        """Specifying no input paths should return no results."""
        root = self.mkdtemp()

        os.mkdir(os.path.join(root, "mydir"))
        self.touchRelative(root, "myfile")
        self.watchmanCommand("watch", root)

        self.assertFileListsEqual(
            self.watchmanCommand("query", root, {"fields": ["name"], "path": []})[
                "files"
            ],
            [],
        )
