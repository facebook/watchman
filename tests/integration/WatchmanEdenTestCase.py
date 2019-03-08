# vim:ts=4:sw=4:et:
# Copyright 2016-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

# no unicode literals
from __future__ import absolute_import, division, print_function

import os
import tempfile

import WatchmanInstance
import WatchmanTestCase


TestParent = object
try:
    import configparser  # python3
    from eden.integration.lib import edenclient, hgrepo

    def is_sandcastle():
        return "SANDCASTLE" in os.environ

    if edenclient.can_run_eden():
        TestParent = WatchmanTestCase.WatchmanTestCase

    can_run_eden = edenclient.can_run_eden

except ImportError as e:

    def is_buck_build():
        return "BUCK_BUILD_ID" in os.environ

    # We want import failures to hard fail the build when using buck internally
    # because it means we overlooked something, but we want it to be a soft
    # fail when we run our opensource build
    if is_buck_build():
        raise

    def can_run_eden():
        return False


class WatchmanEdenTestCase(TestParent):
    # The contents of the .eden directory
    # This is used by several tests when checking reported file lists
    eden_dir_entries = [".eden/root", ".eden/socket", ".eden/client", ".eden/this-dir"]

    def setUp(self):
        super(WatchmanEdenTestCase, self).setUp()

        # the eden home directory.  We use the global dir for the test runner
        # rather than one scoped to the test because we have very real length
        # limits on the socket path name that we're likely to hit otherwise.
        # fake a home dir so that eden is isolated from the settings
        # of the user running these tests.
        self.eden_home = tempfile.mkdtemp(prefix="eden_home")
        self.eden_dir = os.path.join(self.eden_home, "local/.eden")
        os.makedirs(self.eden_dir)

        self.etc_eden_dir = os.path.join(self.eden_home, "etc-eden")
        os.mkdir(self.etc_eden_dir)
        # The directory holding the system configuration files
        self.system_config_dir = os.path.join(self.etc_eden_dir, "config.d")
        os.mkdir(self.system_config_dir)

        # where we'll mount the eden client(s)
        self.mounts_dir = self.mkdtemp(prefix="eden_mounts")

        self.save_home = os.environ["HOME"]
        os.environ["HOME"] = self.eden_home

        # Watchman needs to start up with the same HOME as eden, otherwise
        # it won't be able to locate the eden socket
        self.eden_watchman = WatchmanInstance.Instance()
        self.eden_watchman.start()

        self.client = self.getClient(self.eden_watchman)

        # chg can interfere with eden, so disable it up front
        os.environ["CHGDISABLE"] = "1"
        self.eden = edenclient.EdenFS(
            self.eden_dir, etc_eden_dir=self.etc_eden_dir, home_dir=self.eden_home
        )
        self.eden.start()

    def tearDown(self):
        if self.eden:
            self.cleanUpWatches()
            self.eden.cleanup()

        super(WatchmanEdenTestCase, self).tearDown()

        if self.eden_watchman:
            self.eden_watchman.stop()

        if self.save_home:
            os.environ["HOME"] = self.save_home

    def cleanUpWatches(self):
        roots = self.watchmanCommand("watch-list")["roots"]
        self.watchmanCommand("watch-del-all")
        for root in roots:
            try:
                self.eden.unmount(root)
            except Exception:
                pass

    def makeEdenMount(self, populate_fn=None):
        """ populate_fn is a function that accepts a repo object and
            that is expected to populate it as a pre-requisite to
            starting up the eden mount for it.
        """

        repo_path = self.mkdtemp(prefix="eden_repo_")
        repo_name = os.path.basename(repo_path)
        repo = self.repoForPath(repo_path)
        repo.init()

        if populate_fn:
            populate_fn(repo)

        self.eden.add_repository(repo_name, repo_path)

        mount_path = os.path.join(self.mounts_dir, repo_name)

        self.eden.clone(repo_name, mount_path)
        return mount_path

    def repoForPath(self, path):
        return hgrepo.HgRepository(path)

    def setDefaultConfiguration(self):
        self.setConfiguration("local", "bser")
