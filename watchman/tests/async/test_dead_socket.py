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

import asyncio
import os
import unittest

import pywatchman_aio
import WatchmanInstance


# Note this does not extend AsyncWatchmanTestCase as it wants to start its
# own Watchman server instances per test.
class TestDeadSocket(unittest.TestCase):
    @unittest.skipIf(os.name == "nt", "not supported on windows")
    def test_query_dead_socket(self):
        async def test_core(wminst):
            with await pywatchman_aio.AIOClient.from_socket(
                sockname=wminst.getSockPath()
            ) as client:
                wminst.stop()
                with self.assertRaises(ConnectionResetError):
                    await client.query("version")

        self._async_runner(test_core)

    @unittest.skipIf(os.name == "nt", "not supported on windows")
    def test_subscription_dead_socket(self):
        async def test_core(wminst):
            with await pywatchman_aio.AIOClient.from_socket(
                sockname=wminst.getSockPath()
            ) as client:
                root = f"{wminst.base_dir}/work"
                os.makedirs(root)
                await client.query("watch", root)
                await client.query("subscribe", root, "sub", {"expression": ["exists"]})
                wminst.stop()
                with self.assertRaises(ConnectionResetError):
                    await client.get_subscription("sub", root)

        self._async_runner(test_core)

    def _async_runner(self, test_core):
        wminst = WatchmanInstance.Instance()
        wminst.start()
        try:
            return asyncio.new_event_loop().run_until_complete(test_core(wminst))
        finally:
            wminst.stop()
