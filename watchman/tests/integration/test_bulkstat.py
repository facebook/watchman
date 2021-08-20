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

import WatchmanInstance
import WatchmanTestCase


@WatchmanTestCase.expand_matrix
class TestBulkStat(WatchmanTestCase.WatchmanTestCase):
    def test_bulkstat_on(self):
        config = {"_use_bulkstat": True}
        with WatchmanInstance.Instance(config=config) as inst:
            inst.start()
            self.getClient(inst, replace_cached=True)

            root = self.mkdtemp()
            self.client.query("watch", root)

            self.touchRelative(root, "foo")
            self.touchRelative(root, "bar")
            self.assertFileList(root, ["foo", "bar"])

    def test_bulkstat_off(self):
        config = {"_use_bulkstat": False}
        with WatchmanInstance.Instance(config=config) as inst:
            inst.start()
            self.getClient(inst, replace_cached=True)

            root = self.mkdtemp()
            self.client.query("watch", root)

            self.touchRelative(root, "foo")
            self.touchRelative(root, "bar")
            self.assertFileList(root, ["foo", "bar"])
