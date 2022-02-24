# vim:ts=4:sw=4:et:
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# no unicode literals

import os
import shutil

from watchman.integration.lib import WatchmanEdenTestCase


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

    def query_adir_change_since(self, root, clock):
        return self.watchmanCommand(
            "query",
            root,
            {
                "expression": [
                    "anyof",
                    ["match", "adir", "basename"],
                    ["dirname", "adir"],
                ],
                "fields": ["name", "type"],
                "since": clock,
                "empty_on_fresh_instance": True,
                "always_include_directories": True,
            },
        )

    def test_eden_since_removal(self):
        root = self.makeEdenMount(populate)

        res = self.watchmanCommand("watch", root)
        self.assertEqual("eden", res["watcher"])

        first_clock = self.watchmanCommand(
            "clock",
            root,
        )["clock"]

        shutil.rmtree(os.path.join(root, "adir"))

        first_res = self.query_adir_change_since(root, first_clock)

        # TODO(T104564495): fix this incorrect behavior.
        # we are asserting some behavior that deviates from non eden watchman
        # here: "adir" is an "f" type.
        # watchman tries to check the type of a file after it gets the
        # notification that that file changed. That is useless for removals.
        # We will never be able to report the type of file removed on eden.
        # We should fix this, but for now Watchman just reports unknown types
        # as f.
        self.assertQueryRepsonseEqual(
            [{"name": "adir", "type": "f"}, {"name": "adir/file", "type": "f"}],
            first_res["files"],
        )

    def test_eden_since_across_update(self):
        root = self.makeEdenMount(populate)
        repo = self.repoForPath(root)

        res = self.watchmanCommand("watch", root)
        self.assertEqual("eden", res["watcher"])

        shutil.rmtree(os.path.join(root, "adir"))

        # commit the removal so we can test the change across an update.
        repo.hg("addremove")
        repo.commit("removal commit.")

        first_clock = self.watchmanCommand(
            "clock",
            root,
        )["clock"]

        repo.hg("prev")  # add the files back across commits

        first_res = self.query_adir_change_since(root, first_clock)

        self.assertQueryRepsonseEqual(
            [{"name": "adir", "type": "d"}, {"name": "adir/file", "type": "f"}],
            first_res["files"],
        )

        second_clock = self.watchmanCommand(
            "clock",
            root,
        )["clock"]

        repo.hg("next")  # remove the files again across commits

        second_res = self.query_adir_change_since(root, second_clock)

        self.assertQueryRepsonseEqual(
            [{"name": "adir", "type": "f"}, {"name": "adir/file", "type": "f"}],
            second_res["files"],
        )
