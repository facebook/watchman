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

import pywatchman
import WatchmanTestCase


@WatchmanTestCase.expand_matrix
class TestPcre(WatchmanTestCase.WatchmanTestCase):
    def check_pcre(self):
        res = self.watchmanCommand("version", {"optional": ["term-pcre"]})
        if not res["capabilities"]["term-pcre"]:
            self.skipTest("no PCRE support in the server")

    def test_big_pcre(self):
        self.check_pcre()

        root = self.mkdtemp()
        self.watchmanCommand("watch", root)

        fill = "lemon\\.php" * 3600
        pcre = (
            "^("
            + "|".join([fill[i : i + 100] for i in range(0, len(fill), 100)])
            + "sss)"
        )

        try:
            self.watchmanCommand(
                "query",
                root,
                {"expression": ["pcre", pcre, "wholename"], "fields": ["name"]},
            )

            # Some PCRE libraries are actually OK with this
            # expression, so we won't always throw an error
        except pywatchman.WatchmanError as e:
            self.assertIn("is too", str(e))

    def test_pcre(self):
        self.check_pcre()

        root = self.mkdtemp()
        self.touchRelative(root, "foo.c")
        self.touchRelative(root, "bar.txt")
        self.watchmanCommand("watch", root)

        self.assertFileList(root, ["bar.txt", "foo.c"])

        out = self.watchmanCommand("find", root, "-p", ".*c$")
        self.assertEqual(1, len(out["files"]))
        self.assertFileListsEqual(["foo.c"], [out["files"][0]["name"]])

        out = self.watchmanCommand("find", root, "-p", ".*txt$")
        self.assertEqual(1, len(out["files"]))
        self.assertFileListsEqual(["bar.txt"], [out["files"][0]["name"]])

        with self.assertRaises(pywatchman.WatchmanError) as ctx:
            self.watchmanCommand("find", root, "-p", "(")
        self.assertIn("at offset 1 in (", str(ctx.exception))

        if self.isCaseInsensitive():
            # -p matches case sensitivity of filesystem
            out = self.watchmanCommand("find", root, "-p", ".*C$")
            self.assertEqual(1, len(out["files"]))
            self.assertFileListsEqual(["foo.c"], [out["files"][0]["name"]])

        # and case insensitive mode
        out = self.watchmanCommand("find", root, "-P", ".*C$")
        self.assertEqual(1, len(out["files"]))
        self.assertFileListsEqual(["foo.c"], [out["files"][0]["name"]])
