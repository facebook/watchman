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
class TestFields(WatchmanTestCase.WatchmanTestCase):
    def test_fields(self):
        root = self.mkdtemp()
        self.watchmanCommand("watch", root)
        self.touchRelative(root, "a")
        self.assertFileList(root, files=["a"])

        result = self.watchmanCommand(
            "query",
            root,
            {
                "fields": [
                    "name",
                    "exists",
                    "new",
                    "size",
                    "mode",
                    "uid",
                    "gid",
                    "mtime",
                    "mtime_ms",
                    "mtime_us",
                    "mtime_ns",
                    "mtime_f",
                    "ctime",
                    "ctime_ms",
                    "ctime_us",
                    "ctime_ns",
                    "ctime_f",
                    "ino",
                    "dev",
                    "nlink",
                    "oclock",
                    "cclock",
                ],
                "since": "n:foo",
            },
        )
        self.assertEqual(len(result["files"]), 1)
        file = result["files"][0]
        self.assertEqual(file["name"], "a")
        self.assertTrue(file["exists"])
        self.assertTrue(file["new"])

        st = os.lstat(os.path.join(root, "a"))

        fields = ["size", "mode", "uid", "gid"]
        if os.name != "nt":
            # These fields are meaningless in msvcrt
            fields += ["dev", "ino"]
            # Python seemingly has different logic for nlink than
            # watchman and php on nt
            fields += ["nlink"]

        for field in fields:
            self.assertEqual(file[field], getattr(st, "st_" + field), msg=field)

        for field in ["mtime", "ctime"]:
            self.assertEqual(file[field], int(getattr(st, "st_" + field)), msg=field)
            seconds = file[field]
            ms = file[field + "_ms"]
            us = file[field + "_us"]
            ns = file[field + "_ns"]
            self.assertEqual(ms // 1000, seconds)
            self.assertEqual(us // 1000, ms)
            self.assertEqual(ns // 1000, us)

        for field in ["cclock", "oclock"]:
            self.assertRegex(file[field], "^c:\\d+:\\d+:\\d+:\\d+$")
