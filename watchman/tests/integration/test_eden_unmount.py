# vim:ts=4:sw=4:et:
# Copyright 2012-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

# no unicode literals
from __future__ import absolute_import, division, print_function

import os

import pywatchman
import WatchmanEdenTestCase


class TestEdenUnmount(WatchmanEdenTestCase.WatchmanEdenTestCase):
    def test_eden_unmount(self):
        def populate(repo):
            repo.write_file(".watchmanconfig", '{"ignore_dirs":[".buckd"]}')
            repo.write_file("hello", "hola\n")
            repo.commit("initial commit.")

        root = self.makeEdenMount(populate)
        self.watchmanCommand("watch", root)

        clock = self.watchmanCommand("clock", root)
        self.touchRelative(root, "newfile")

        self.eden.unmount(root)

        with self.assertRaises(pywatchman.CommandError) as ctx:
            self.watchmanCommand("query", root, {"fields": ["name"], "since": clock})

        self.assertRegex(str(ctx.exception), "unable to resolve root")
