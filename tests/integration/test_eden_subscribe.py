# vim:ts=4:sw=4:et:
# Copyright 2017-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

# no unicode literals
from __future__ import absolute_import, division, print_function

import os

import WatchmanEdenTestCase


class TestEdenSubscribe(WatchmanEdenTestCase.WatchmanEdenTestCase):

    def requiresPersistentSession(self):
        return True

    def test_eden_subscribe(self):

        def populate(repo):
            repo.write_file(".watchmanconfig", '{"ignore_dirs":[".buckd"]}')
            repo.write_file("hello", "hola\n")
            repo.commit("initial commit.")

        root = self.makeEdenMount(populate)

        res = self.watchmanCommand("watch", root)
        self.assertEqual("eden", res["watcher"])

        self.watchmanCommand("subscribe", root, "myname", {"fields": ["name"]})

        dat = self.waitForSub("myname", root=root)[0]
        self.assertTrue(dat["is_fresh_instance"])
        self.assertFileListsEqual(
            self.normWatchmanFileList(dat["files"]),
            self.normFileList(
                [
                    ".eden",
                    ".eden/socket",
                    ".eden/client",
                    ".watchmanconfig",
                    ".eden/root",
                    "hello",
                ]
            ),
        )

        self.touchRelative(root, "w0000t")
        dat = self.waitForSub("myname", root=root)[0]
        self.assertEqual(False, dat["is_fresh_instance"])
        self.assertFileListsEqual(
            self.normWatchmanFileList(dat["files"]), self.normFileList(["w0000t"])
        )

        # we should not observe .buckd in the subscription results
        # because it is listed in the ignore_dirs config section.
        os.mkdir(os.path.join(root, ".buckd"))

        self.touchRelative(root, "hello")
        dat = self.waitForSub("myname", root=root)[0]
        self.assertEqual(False, dat["is_fresh_instance"])
        self.assertFileListsEqual(
            self.normWatchmanFileList(dat["files"]), self.normFileList(["hello"])
        )

        # make another subscription and assert that we get a fresh
        # instance result with all the files in it
        self.watchmanCommand("subscribe", root, "othersub", {"fields": ["name"]})

        dat = self.waitForSub("othersub", root=root)[0]
        self.assertEqual(True, dat["is_fresh_instance"])
        self.assertFileListsEqual(
            self.normWatchmanFileList(dat["files"]),
            self.normFileList(
                [
                    ".eden",
                    ".eden/socket",
                    ".eden/client",
                    ".watchmanconfig",
                    ".eden/root",
                    "hello",
                    "w0000t",
                ]
            ),
        )

    def assertWaitForAssertedStates(self, root, states):

        def sortStates(states):
            """ Deterministically sort the states for comparison.
            We sort by name and rely on the sort being stable as the
            relative ordering of the potentially multiple queueued
            entries per name is important to preserve """
            return sorted(states, key=lambda x: x["name"])

        states = sortStates(states)

        def getStates():
            res = self.watchmanCommand("debug-get-asserted-states", root)
            return sortStates(res["states"])

        self.assertWaitForEqual(states, getStates)

    def test_state_enter_leave(self):
        """ Check that state-enter and state-leave are basically working.
        This is a subset of the tests that are performed in test_subscribe.py;
        we only strictly need to check the basic plumbing here and need not
        replicate the entire set of tests"""

        def populate(repo):
            repo.write_file("hello", "hola\n")
            repo.commit("initial commit.")

        root = self.makeEdenMount(populate)
        res = self.watchmanCommand("watch", root)
        self.assertEqual("eden", res["watcher"])

        result = self.watchmanCommand("debug-get-asserted-states", root)
        self.assertEqual([], result["states"])

        self.watchmanCommand("state-enter", root, "foo")
        self.watchmanCommand("state-enter", root, "bar")
        self.assertWaitForAssertedStates(
            root,
            [
                {"name": "bar", "state": "Asserted"},
                {"name": "foo", "state": "Asserted"},
            ],
        )

        self.watchmanCommand("state-leave", root, "foo")
        self.assertWaitForAssertedStates(root, [{"name": "bar", "state": "Asserted"}])

        self.watchmanCommand("state-leave", root, "bar")
        self.assertWaitForAssertedStates(root, [])
