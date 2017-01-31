# vim:ts=4:sw=4:et:
# Copyright 2016-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import WatchmanTestCase
import WatchmanInstance
import os
import tempfile
try:
    import unittest2 as unittest
except ImportError:
    import unittest

TestParent = object
try:
    from eden.integration.lib import (edenclient, hgrepo)

    def is_sandcastle():
        return 'SANDCASTLE' in os.environ

    if not is_sandcastle() and edenclient.can_run_eden():
        TestParent = WatchmanTestCase.WatchmanTestCase

    can_run_eden = edenclient.can_run_eden

except ImportError:
    def can_run_eden():
        return False


class WatchmanEdenTestCase(TestParent):
    def setUp(self):
        # the eden home directory.  We use the global dir for the test runner
        # rather than one scoped to the test because we have very real length
        # limits on the socket path name that we're likely to hit otherwise.
        # fake a home dir so that eden is isolated from the settings
        # of the user running these tests.
        self.eden_home = tempfile.mkdtemp(prefix='eden_home')
        self.eden_dir = os.path.join(self.eden_home, 'local/.eden')
        os.makedirs(self.eden_dir)

        self.etc_eden_dir = os.path.join(self.eden_home, 'etc-eden')
        os.mkdir(self.etc_eden_dir)
        # The directory holding the system configuration files
        self.system_config_dir = os.path.join(self.etc_eden_dir, 'config.d')
        os.mkdir(self.system_config_dir)

        # where we'll mount the eden client(s)
        self.mounts_dir = self.mkdtemp(prefix='eden_mounts')

        self.save_home = os.environ['HOME']
        os.environ['HOME'] = self.eden_home
        self.eden = edenclient.EdenFS(
            self.eden_dir,
            etc_eden_dir=self.etc_eden_dir,
            home_dir=self.eden_home
        )
        self.eden.start()

        # Watchman also needs to start up with the same HOME, otherwise
        # it won't be able to locate the eden socket
        self.eden_watchman = WatchmanInstance.Instance()
        self.eden_watchman.start()

        self.client = self.getClient(self.eden_watchman)

    def tearDown(self):
        if self.save_home:
            os.environ['HOME'] = self.save_home

        if self.eden:
            self.eden.cleanup()

        if self.eden_watchman:
            self.eden_watchman.stop()

    def makeEdenMount(self, populate_fn=None):
        ''' populate_fn is a function that accepts a repo object and
            that is expected to populate it as a pre-requisite to
            starting up the eden mount for it. '''

        repo_path = self.mkdtemp(prefix='eden_repo_')
        repo_name = os.path.basename(repo_path)
        repo = hgrepo.HgRepository(repo_path)
        repo.init()

        if populate_fn:
            populate_fn(repo)

        self.eden.add_repository(repo_name, repo_path)

        mount_path = os.path.join(self.mounts_dir, repo_name)
        self.eden.clone(repo_name, mount_path)
        return mount_path

    def setDefaultConfiguration(self):
        self.setConfiguration('local', 'bser')
