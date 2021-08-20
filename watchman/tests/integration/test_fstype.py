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
import WatchmanInstance
import WatchmanTestCase


@WatchmanTestCase.expand_matrix
class TestIllegalFSType(WatchmanTestCase.WatchmanTestCase):
    def test_Illegal(self):
        config = {
            "illegal_fstypes": [
                # This should include any/all fs types. If this test fails on
                # your platform, look in /tmp/watchman-test.log for a line like:
                # "path /var/tmp/a3osdzvzqnco0sok is on filesystem type zfs"
                # then add the type name to this list, in sorted order
                "NTFS",
                "apfs",
                "btrfs",
                "cifs",
                "ext2",
                "ext3",
                "ext4",
                "fuse",
                "hfs",
                "msdos",
                "nfs",
                "smb",
                "tmpfs",
                "ufs",
                "unknown",
                "xfs",
                "zfs",
            ],
            "illegal_fstypes_advice": "just cos",
        }

        with WatchmanInstance.Instance(config=config) as inst:
            inst.start()
            client = self.getClient(inst)

            d = self.mkdtemp()
            with self.assertRaises(pywatchman.WatchmanError) as ctx:
                client.query("watch", d)
            self.assertIn(
                (
                    "filesystem and is disallowed by global config"
                    + " illegal_fstypes: just cos"
                ),
                str(ctx.exception),
            )
