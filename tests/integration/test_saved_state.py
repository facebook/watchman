# vim:ts=4:sw=4:et:
# Copyright 2017-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

# no unicode literals
from __future__ import absolute_import, division, print_function

import pywatchman
import WatchmanSCMTestCase
import WatchmanTestCase


if pywatchman.compat.PYTHON3:
    STRING_TYPES = (str, bytes)
else:
    STRING_TYPES = (str, unicode)  # noqa: F821


@WatchmanTestCase.expand_matrix
class TestSavedState(WatchmanSCMTestCase.WatchmanSCMTestCase):
    def setUp(self):
        self.skipIfNoFSMonitor()
        self.root = self.mkdtemp()
        # This test does not require much so just create a super simple repo
        self.hg(["init"], cwd=self.root)
        self.touchRelative(self.root, "foo")
        self.hg(["book", "initial"], cwd=self.root)
        self.hg(["addremove"], cwd=self.root)
        self.hg(["commit", "-m", "initial"], cwd=self.root)
        self.touchRelative(self.root, "bar")
        self.touchRelative(self.root, "car")
        self.hg(["addremove"], cwd=self.root)
        self.hg(["commit", "-m", "add bar and car"], cwd=self.root)
        self.hg(["book", "TheMaster"], cwd=self.root)
        self.watchmanCommand("watch", self.root)

    def get_skeleton_query(self):
        return {
            "expression": [
                "not",
                ["anyof", ["name", ".hg"], ["match", "hg-check*"], ["dirname", ".hg"]],
            ],
            "fields": ["name"],
            "since": {"scm": {"mergebase-with": "TheMaster"}},
        }

    def test_unsupportedStorageType(self):
        # If the storage type is not supported, watchman should throw
        test_query = self.get_skeleton_query()
        test_query["since"]["scm"]["saved-state"] = {"storage": "foo", "config": {}}
        with self.assertRaises(pywatchman.WatchmanError) as ctx:
            self.watchmanCommand("query", self.root, test_query)
        self.assertIn("invalid storage type 'foo'", str(ctx.exception))
