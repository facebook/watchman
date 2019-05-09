# vim:ts=4:sw=4:et:
# Copyright 2012-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

# no unicode literals
from __future__ import absolute_import, division, print_function

import os
import subprocess

import WatchmanTestCase


try:
    from shlex import quote as shellquote
except ImportError:
    from pipes import quote as shellquote


@WatchmanTestCase.expand_matrix
class TestFishy(WatchmanTestCase.WatchmanTestCase):
    def checkOSApplicability(self):
        if os.name == "nt":
            self.skipTest("non admin symlinks and unix userland not available")

    def test_fishy(self):
        root = self.mkdtemp()

        os.mkdir(os.path.join(root, "foo"))
        self.touchRelative(root, "foo", "a")

        self.watchmanCommand("watch", root)
        base = self.watchmanCommand("find", root, ".")
        clock = base["clock"]

        self.suspendWatchman()
        # Explicitly using the shell to run these commands
        # as the original test case wasn't able to reproduce
        # the problem with whatever sequence and timing of
        # operations was produced by the original php test
        subprocess.check_call(
            "cd %s && mv foo bar && ln -s bar foo" % shellquote(root), shell=True
        )

        self.resumeWatchman()
        self.assertFileList(root, files=["bar", "bar/a", "foo"], cursor=clock)

    def test_more_moves(self):
        root = self.mkdtemp()
        self.watchmanCommand("watch", root)
        base = self.watchmanCommand("find", root, ".")
        clock = base["clock"]

        self.suspendWatchman()
        # Explicitly using the shell to run these commands
        # as the original test case wasn't able to reproduce
        # the problem with whatever sequence and timing of
        # operations was produced by the original php test
        subprocess.check_call(
            "cd %s && touch a && mkdir d1 d2 && mv d1 d2 && mv d2/d1 . && mv a d1"
            % shellquote(root),
            shell=True,
        )
        self.resumeWatchman()
        self.assertFileList(root, files=["d1", "d1/a", "d2"], cursor=clock)

    def test_even_more_moves(self):
        root = self.mkdtemp()
        self.watchmanCommand("watch", root)
        base = self.watchmanCommand("find", root, ".")
        clock = base["clock"]

        self.suspendWatchman()
        # Explicitly using the shell to run these commands
        # as the original test case wasn't able to reproduce
        # the problem with whatever sequence and timing of
        # operations was produced by the original php test
        subprocess.check_call(
            (
                "cd %s && "
                "mkdir d1 d2 && "
                "touch d1/a && "
                "mkdir d3 && "
                "mv d1 d2 d3 && "
                "mv d3/* . && "
                "mv d1 d2 d3 && "
                "mv d3/* . && "
                "mv d1/a d2"
            )
            % shellquote(root),
            shell=True,
        )
        self.resumeWatchman()
        self.assertFileList(root, files=["d1", "d2", "d2/a", "d3"], cursor=clock)

    def test_notify_dir(self):
        root = self.mkdtemp()
        self.watchmanCommand("watch", root)
        os.mkdir(os.path.join(root, "wtest"))
        os.mkdir(os.path.join(root, "wtest", "dir"))
        self.touchRelative(root, "wtest", "1")
        self.touchRelative(root, "wtest", "2")
        self.assertFileList(
            root, ["wtest", "wtest/1", "wtest/2", "wtest/dir"], cursor="n:foo"
        )

        os.rmdir(os.path.join(root, "wtest/dir"))
        files = self.watchmanCommand(
            "query", root, {"fields": ["name"], "since": "n:foo"}
        )["files"]
        self.assertFileListsEqual(files, ["wtest", "wtest/dir"])

        os.unlink(os.path.join(root, "wtest/2"))
        files = self.watchmanCommand(
            "query", root, {"fields": ["name"], "since": "n:foo"}
        )["files"]
        self.assertFileListsEqual(files, ["wtest", "wtest/2"])

        os.unlink(os.path.join(root, "wtest/1"))
        files = self.watchmanCommand(
            "query", root, {"fields": ["name"], "since": "n:foo"}
        )["files"]
        self.assertFileListsEqual(files, ["wtest", "wtest/1"])
