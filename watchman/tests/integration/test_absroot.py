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
import os.path

import pywatchman
import WatchmanTestCase
from path_utils import norm_absolute_path


@WatchmanTestCase.expand_matrix
class TestAbsoluteRoot(WatchmanTestCase.WatchmanTestCase):
    def test_dot(self):
        root = self.mkdtemp()

        save_dir = os.getcwd()
        try:
            os.chdir(root)

            dot = "" if os.name == "nt" else "."

            if self.transport == "cli":
                res = self.watchmanCommand("watch", dot)
                self.assertEqual(root, norm_absolute_path(res["watch"]))
            else:
                with self.assertRaises(pywatchman.WatchmanError) as ctx:
                    self.watchmanCommand("watch", dot)

                self.assertIn("must be absolute", str(ctx.exception))

        finally:
            os.chdir(save_dir)

    def test_root(self):
        if os.name != "nt":
            with self.assertRaises(pywatchman.WatchmanError) as ctx:
                self.watchmanCommand("watch", "/")

                self.assertIn("cannot watch", str(ctx.exception))
