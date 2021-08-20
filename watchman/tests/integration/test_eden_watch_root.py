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
import WatchmanEdenTestCase


class TestEdenWatchRoot(WatchmanEdenTestCase.WatchmanEdenTestCase):
    def test_eden_watch_root(self):
        def populate(repo):
            repo.write_file("adir/file", "foo!\n")
            repo.commit("initial commit.")

        root = self.makeEdenMount(populate)

        with self.assertRaises(pywatchman.WatchmanError) as ctx:
            self.watchmanCommand("watch", os.path.join(root, "adir"))
        self.assertRegex(
            str(ctx.exception),
            (
                "unable to resolve root .*: eden: you may only watch "
                + "from the root of an eden mount point."
            ),
        )
