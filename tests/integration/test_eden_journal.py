# vim:ts=4:sw=4:et:
# Copyright 2017-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

# no unicode literals
from __future__ import absolute_import, division, print_function

import time

import WatchmanEdenTestCase


class TestEdenJournal(WatchmanEdenTestCase.WatchmanEdenTestCase):
    def test_eden_journal(self):
        def populate(repo):
            repo.write_file("hello", "hola\n")
            repo.commit("initial commit.")

        root = self.makeEdenMount(populate, enable_hg=True)
        repo = self.repoForPath(root)
        initial_commit = repo.get_head_hash()

        res = self.watchmanCommand("watch", root)
        self.assertEqual("eden", res["watcher"])

        clock = self.watchmanCommand("clock", root)

        self.touchRelative(root, "newfile")

        res = self.watchmanCommand("query", root, {"fields": ["name"], "since": clock})
        clock = res["clock"]
        self.assertFileListsEqual(res["files"], ["newfile"])

        repo.add_file("newfile")
        repo.commit(message="add newfile")
        res = self.watchmanCommand(
            "query",
            root,
            {
                "expression": [
                    "not",
                    [
                        "anyof",
                        ["dirname", ".hg"],
                        ["match", "checklink*"],
                        ["match", "hg-check*"],
                    ],
                ],
                "fields": ["name"],
                "since": clock,
            },
        )
        clock = res["clock"]
        self.assertFileListsEqual(
            res["files"],
            ["newfile"],
            message="We expect to report the files changed in the commit",
        )

        # Test the the journal has the correct contents across a "reset" like
        # operation where the parents are poked directly.   This is using
        # debugsetparents rather than reset because the latter isn't enabled
        # by default for hg in the watchman test machinery.
        self.touchRelative(root, "unclean")
        repo.hg("debugsetparents", initial_commit)
        res = self.watchmanCommand(
            "query",
            root,
            {
                "expression": ["not", ["dirname", ".hg"]],
                "fields": ["name"],
                "since": clock,
            },
        )
        self.assertFileListsEqual(
            res["files"],
            ["newfile", "unclean"],
            message=(
                "We expect to report the file changed in the commit "
                "as well as the unclean file"
            ),
        )

        # make sure that we detect eden getting unmounted.  This sleep is unfortunate
        # and ugly.  Without it, the unmount will fail because something is accessing
        # the filesystem.  I haven't been able to find out what it is because fuser
        # takes too long to run and by the time it has run, whatever that blocker
        # was is not longer there.  Ordinarily I'd prefer to poll on some condition
        # in a loop rather than just sleeping an arbitrary amount, but I just don't
        # know what the offending thing is and running the unmount in a loop is prone
        # to false negatives.
        time.sleep(1)

        self.eden.remove(root)
        watches = self.watchmanCommand("watch-list")
        self.assertNotIn(root, watches["roots"])
