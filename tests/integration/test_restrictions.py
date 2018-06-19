# vim:ts=4:sw=4:et:
# Copyright 2016-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

# no unicode literals
from __future__ import absolute_import, division, print_function

import json
import os

import pywatchman
import WatchmanInstance
import WatchmanTestCase


@WatchmanTestCase.expand_matrix
class TestWatchRestrictions(WatchmanTestCase.WatchmanTestCase):
    def test_rootRestrict(self):
        config = {"root_restrict_files": [".git", ".foo"]}

        inst = WatchmanInstance.Instance(config=config)
        try:
            inst.start()
            client = self.getClient(inst)

            expect = [
                ("directory", ".git", True),
                ("file", ".foo", True),
                ("directory", ".foo", True),
                (None, None, False),
                ("directory", ".svn", False),
                ("file", "baz", False),
            ]

            for filetype, name, expect_pass in expect:
                # encode the test criteria in the dirname so that we can
                # figure out which test scenario failed more easily
                d = self.mkdtemp(suffix="-%s-%s-%s" % (filetype, name, expect_pass))
                if filetype == "directory":
                    os.mkdir(os.path.join(d, name))
                elif filetype == "file":
                    self.touchRelative(d, name)

                if expect_pass:
                    self.assertWatchSucceeds(client, d)
                else:
                    self.assertWatchIsRestricted(client, d)

        finally:
            inst.stop()

    def assertWatchSucceeds(self, client, path):
        client.query("watch", path)

    def assertWatchIsRestricted(self, client, path):
        with self.assertRaises(pywatchman.WatchmanError) as ctx:
            client.query("watch", path)
        self.assertRegex(
            str(ctx.exception),
            (
                "unable to resolve root .*"
                + ": Your watchman administrator has configured watchman "
                + "to prevent watching path `.*`.  "
                + "None of the files listed in global config root_files "
                + "are present and enforce_root_files is set to true.  "
                + "root_files is defined by the `.*` config file and "
                + "includes `.watchmanconfig`, `.git`, and "
                + "`.foo`.  One or more of these files must be "
                + "present in order to allow a watch.  Try pulling "
                + "and checking out a newer version of the project?"
            ),
        )

    def test_invalidRoot(self):
        d = self.mkdtemp()
        invalid = os.path.join(d, "invalid")
        with self.assertRaises(pywatchman.WatchmanError) as ctx:
            self.watchmanCommand("watch", invalid)
        msg = str(ctx.exception)
        if "No such file or directory" in msg:
            # unix
            return
        if "The system cannot find the file specified" in msg:
            # windows
            return
        self.assertTrue(False, msg)
