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
        expect = [
            ("directory", ".git", True),
            ("file", ".foo", True),
            ("directory", ".foo", True),
            (None, None, False),
            ("directory", ".svn", False),
            ("file", "baz", False),
        ]
        self.runWatchTests(config=config, expect=expect)

    def runWatchTests(self, config, expect):
        with WatchmanInstance.Instance(config=config) as inst:
            inst.start()
            client = self.getClient(inst)

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

    def assertWatchSucceeds(self, client, path):
        client.query("watch", path)

    def assertWatchIsRestricted(self, client, path):
        with self.assertRaises(pywatchman.WatchmanError) as ctx:
            client.query("watch", path)
        message = str(ctx.exception)
        self.assertIn("unable to resolve root {}".format(path), message)
        self.assertIn(
            (
                "Your watchman administrator has configured watchman to "
                + "prevent watching path `{}`"
            ).format(path),
            message,
        )
        self.assertIn(
            "None of the files listed in global config root_files are present "
            + "and enforce_root_files is set to true.",
            message,
        )
        self.assertRegex(message, "root_files is defined by the `.*?` config file")
        self.assertIn(
            "config file and includes `.watchmanconfig`, `.git`, and `.foo`.", message
        )
        self.assertIn(
            "One or more of these files must be present in order to allow a "
            + "watch.  Try pulling and checking out a newer version of the "
            + "project?",
            message,
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
