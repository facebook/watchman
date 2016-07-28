# Copyright 2012-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

import errno
import unittest
import time
import os.path
import os
import WatchmanInstance
import copy
import sys
import glob
import asyncio

from pywatchman.aioclient import AIOClient as WatchmanClient
from pywatchman import encoding

import pywatchman

def norm_path(name):
    return os.path.normcase(os.path.normpath(name))

# TODO: This normalization will not be needed once we have full unicode support
# in place as per
# https://github.com/facebook/watchman/wiki/Better-Unicode-handling-plan
def conv_path_to_bytes(path):
    if isinstance(path, str):
        return path.encode('utf8')
    else:
        return path

class AsyncWatchmanTestCase(unittest.TestCase):

    def setUp(self):
        self.loop = asyncio.get_event_loop()
        self.client = self.loop.run_until_complete(WatchmanClient.from_socket())

    def tearDown(self):
        self.client.close()

    def run(self, result):
        assert result
        super(AsyncWatchmanTestCase, self).run(result)
        return result

    def touch(self, fname, times=None):
        try:
            os.utime(fname, times)
        except OSError as e:
            if e.errno == errno.ENOENT:
                with open(fname, 'a'):
                    os.utime(fname, times)
            else:
                raise

    def touch_relative(self, base, *fname):
        fname = os.path.join(base, *fname)
        self.touch(fname, None)

    def watchman_command(self, *args):

        task = asyncio.wait_for(self.client.query(*args), 10)
        return self.loop.run_until_complete(task)

    def get_file_list(self, root):
        expr = {
            "expression": ["exists"],
            "fields": ["name"],
        }
        res = self.watchman_command('query', root, expr)['files']
        return res

    def assert_sub_contains_all(self, sub, what):
        files = set(conv_path_to_bytes(f) for f in sub['files'])
        for obj in what:
            obj = conv_path_to_bytes(obj)
            assert obj in files, str(obj) + ' was not in subscription'

    def assert_file_sets_equal(self, iter1, iter2, message=None):
        set1 = set(conv_path_to_bytes(f) for f in iter1)
        set2 = set(conv_path_to_bytes(f) for f in iter2)
        self.assertEqual(set1, set2, message)

    # Wait for the file list to match the input set
    def assert_root_file_set(self, root, files):
        self.assert_file_sets_equal(self.get_file_list(root), files)

    def wait_for_sub(self, name, root, timeout=10):
        client = self.client
        task = asyncio.wait_for(client.get_subscription(name, root), timeout)
        return self.loop.run_until_complete(task)

