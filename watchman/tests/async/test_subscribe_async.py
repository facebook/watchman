#!/usr/bin/env python3
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
import tempfile
import unittest

import AsyncWatchmanTestCase


class TestSubscribe(AsyncWatchmanTestCase.AsyncWatchmanTestCase):
    @unittest.skipIf(os.name == "nt", "not supported on windows")
    def test_subscribe(self):
        root = tempfile.mkdtemp()
        a_dir = os.path.join(root, "a")
        os.mkdir(a_dir)
        self.touch_relative(a_dir, "lemon")
        self.touch_relative(root, "b")

        self.watchman_command("watch", root)
        self.assert_root_file_set(root, files=["a", "a/lemon", "b"])

        self.watchman_command("subscribe", root, "myname", {"fields": ["name"]})

        self.watchman_command(
            "subscribe", root, "relative", {"fields": ["name"], "relative_root": "a"}
        )

        # prove initial results come through
        dat = self.wait_for_sub("myname", root=root)
        self.assertEqual(True, dat["is_fresh_instance"])
        self.assert_file_sets_equal(dat["files"], ["a", "a/lemon", "b"])

        # and that relative_root adapts the path name
        dat = self.wait_for_sub("relative", root=root)
        self.assertEqual(True, dat["is_fresh_instance"])
        self.assert_file_sets_equal(dat["files"], ["lemon"])

        # check that deletes show up in the subscription results
        os.unlink(os.path.join(root, "a", "lemon"))
        dat = self.wait_for_sub("myname", root=root)
        self.assertNotEqual(None, dat)
        self.assertEqual(False, dat["is_fresh_instance"])
        self.assert_sub_contains_all(dat, ["a/lemon"])

        dat = self.wait_for_sub("relative", root=root)
        self.assertNotEqual(None, dat)
        self.assertEqual(False, dat["is_fresh_instance"])
        self.assert_sub_contains_all(dat, ["lemon"])

        # Trigger a recrawl and ensure that the subscription isn't lost
        self.watchman_command("debug-recrawl", root)
        # Touch a file to make sure clock increases and subscribtion event.
        # This prevents test failure on some platforms
        self.touch_relative(root, "c")

        dat = self.wait_for_sub("myname", root=root)
        self.assertNotEqual(None, dat)
        self.assertEqual(False, dat["is_fresh_instance"])

        # Ensure that we observed the recrawl warning
        warn = None
        if "warning" in dat:
            warn = dat["warning"]
        self.assertRegex(warn, r"Recrawled this watch")
