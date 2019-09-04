# vim:ts=4:sw=4:et:
# Copyright 2016-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

# no unicode literals
from __future__ import absolute_import, division, print_function

import os

import WatchmanEdenTestCase


def populate(repo):
    # We ignore ".hg" here just so some of the tests that list files don't have to
    # explicitly filter out the contents of this directory.  However, in most situations
    # the .hg directory normally should not be ignored.
    repo.write_file(".watchmanconfig", '{"ignore_dirs":[".hg"]}')
    repo.write_file("hello", "hola\n")
    repo.write_file("adir/file", "foo!\n")
    repo.write_file("bdir/test.sh", "#!/bin/bash\necho test\n", mode=0o755)
    repo.write_file("bdir/noexec.sh", "#!/bin/bash\necho test\n")
    repo.symlink("slink", "hello")
    repo.commit("initial commit.")


class TestEdenSince(WatchmanEdenTestCase.WatchmanEdenTestCase):
    def test_eden_lazy_eval(self):
        root = self.makeEdenMount(populate)
        res = self.watchmanCommand("watch", root)
        self.assertEqual("eden", res["watcher"])

        res = self.watchmanCommand(
            "query",
            root,
            {
                "expression": ["allof", ["type", "f"], ["match", "*.sh"]],
                "fields": ["name"],
                "since": "c:0:0",
            },
        )
        self.assertFileListsEqual(res["files"], ["bdir/test.sh", "bdir/noexec.sh"])

    def test_eden_empty_relative_root(self):
        root = self.makeEdenMount(populate)
        res = self.watchmanCommand("watch", root)
        self.assertEqual("eden", res["watcher"])

        res = self.watchmanCommand(
            "query",
            root,
            {
                "expression": ["type", "f"],
                "relative_root": "",
                "fields": ["name"],
                "since": "c:0:0",
            },
        )

        self.assertFileListsEqual(
            res["files"],
            [".watchmanconfig", "hello", "adir/file", "bdir/test.sh", "bdir/noexec.sh"],
        )

    def test_eden_since(self):
        root = self.makeEdenMount(populate)
        res = self.watchmanCommand("watch", root)
        self.assertEqual("eden", res["watcher"])

        res = self.watchmanCommand(
            "query",
            root,
            {"expression": ["type", "f"], "fields": ["name"], "since": "c:0:0"},
        )
        self.assertTrue(res["is_fresh_instance"])
        self.assertFileListsEqual(
            res["files"],
            ["hello", "adir/file", "bdir/test.sh", "bdir/noexec.sh", ".watchmanconfig"],
        )

        res = self.watchmanCommand(
            "query",
            root,
            {
                "expression": ["type", "f"],
                "relative_root": "adir",
                "fields": ["name"],
                "since": "c:0:0",
            },
        )

        self.assertFileListsEqual(
            res["files"],
            ["file"],
            message="should only return adir/file with no adir prefix",
        )

        clock = res["clock"]

        self.touchRelative(root, "hello")
        res = self.watchmanCommand(
            "query",
            root,
            {"expression": ["type", "f"], "fields": ["name"], "since": clock},
        )
        self.assertFileListsEqual(res["files"], ["hello"])

        res = self.watchmanCommand(
            "query",
            root,
            {"expression": ["type", "f"], "fields": ["name", "new"], "since": clock},
        )
        self.assertEqual([{"name": "hello", "new": False}], res["files"])
        self.touchRelative(root, "hello")

        res = self.watchmanCommand(
            "query",
            root,
            {
                "expression": ["type", "f"],
                "fields": ["name", "new"],
                "since": res["clock"],
            },
        )
        self.assertEqual([{"name": "hello", "new": False}], res["files"])

        res = self.watchmanCommand(
            "query",
            root,
            {"expression": ["type", "f"], "fields": ["name"], "since": res["clock"]},
        )
        self.assertFileListsEqual(res["files"], [])

        res = self.watchmanCommand(
            "query",
            root,
            {
                "expression": ["type", "f"],
                "empty_on_fresh_instance": True,
                "fields": ["name"],
                "since": "c:0:0",
            },
        )
        self.assertTrue(res["is_fresh_instance"])
        self.assertFileListsEqual(res["files"], [])

        os.unlink(os.path.join(root, "hello"))
        res = self.watchmanCommand(
            "query", root, {"fields": ["name"], "since": res["clock"]}
        )
        self.assertFileListsEqual(res["files"], ["hello"])

        res = self.watchmanCommand(
            "query",
            root,
            {"expression": ["type", "f"], "fields": ["name"], "since": res["clock"]},
        )
        self.assertFileListsEqual(res["files"], [])

        self.touchRelative(root, "newfile")
        res = self.watchmanCommand(
            "query",
            root,
            {
                "expression": ["type", "f"],
                "fields": ["name", "new"],
                "since": res["clock"],
            },
        )
        self.assertEqual([{"name": "newfile", "new": True}], res["files"])

        self.touchRelative(root, "newfile")
        res = self.watchmanCommand(
            "query",
            root,
            {
                "expression": ["type", "f"],
                "fields": ["name", "new"],
                "since": res["clock"],
            },
        )
        self.assertEqual([{"name": "newfile", "new": False}], res["files"])

        adir_file = os.path.join(root, "adir/file")
        os.unlink(adir_file)
        with open(adir_file, "w") as f:
            f.write("new contents\n")
        res = self.watchmanCommand(
            "query",
            root,
            {
                "expression": ["type", "f"],
                "fields": ["name", "new"],
                "since": res["clock"],
            },
        )
        self.assertEqual([{"name": "adir/file", "new": False}], res["files"])
