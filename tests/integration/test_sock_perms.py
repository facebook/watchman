# vim:ts=4:sw=4:et:
# Copyright 2016-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import WatchmanInstance
import WatchmanTestCase
try:
    import grp
except ImportError:
    # Windows
    pass
import os
import random
import stat
import string
import tempfile
import time
try:
    import unittest2 as unittest
except ImportError:
    import unittest

import pywatchman

@unittest.skipIf(os.name == 'nt' or os.geteuid() == 0, "win or root")
class TestSockPerms(unittest.TestCase):
    def _new_instance(self, config, expect_success=True):
        if expect_success:
            start_timeout = 1.0
        else:
            # If the instance is going to fail anyway then there's no point
            # waiting so long
            start_timeout = 0.2
        return WatchmanInstance.InstanceWithStateDir(
            config=config, start_timeout=start_timeout)

    def _get_custom_gid(self):
        # This is a bit hard to do: we need to find a group the user is a member
        # of that's not the effective or real gid. If there are none then we
        # must skip.
        groups = os.getgroups()
        for gid in groups:
            if gid != os.getgid() and gid != os.getegid():
                return gid
        self.skipTest('no usable groups found')

    def waitFor(self, cond, timeout=10):
        deadline = time.time() + timeout
        res = None
        while time.time() < deadline:
            res = cond()
            if res:
                return [True, res]
            time.sleep(0.03)
        return [False, res]

    def assertWaitFor(self, cond, timeout=10, message=None):
        status, res = self.waitFor(cond, timeout)
        if status:
            return res
        if message is None:
            message = "%s was not met in %s seconds: %s" % (cond, timeout, res)
        self.fail(message)

    def test_too_open_user_dir(self):
        instance = self._new_instance({}, expect_success=False)
        os.makedirs(instance.user_dir)
        os.chmod(instance.user_dir, 0o777)
        with self.assertRaises(pywatchman.SocketConnectError) as ctx:
            instance.start()
        self.assertEqual(ctx.exception.sockpath, instance.getSockPath())

        wanted = 'the permissions on %s allow others to write to it' % (
            instance.user_dir)
        self.assertWaitFor(lambda: wanted in instance.getCLILogContents())

    def test_invalid_sock_group(self):
        # create a random group name
        while True:
            group_name = ''.join(random.choice(string.ascii_lowercase)
                                 for _ in range(8))
            try:
                grp.getgrnam(group_name)
            except KeyError:
                break

        instance = self._new_instance({'sock_group': group_name},
                                      expect_success=False)
        with self.assertRaises(pywatchman.SocketConnectError) as ctx:
            instance.start()
        self.assertEqual(ctx.exception.sockpath, instance.getSockPath())
        wanted = "group '%s' does not exist" % group_name
        self.assertWaitFor(lambda: wanted in instance.getCLILogContents())

    def test_default_sock_group(self):
        # By default the socket group should be the effective gid of the process
        gid = os.getegid()
        instance = self._new_instance({})
        instance.start()
        instance.stop()

        self.assertFileGID(instance.user_dir, gid)
        self.assertFileGID(instance.sock_file, gid)

    def test_custom_sock_group(self):
        gid = self._get_custom_gid()
        group = grp.getgrgid(gid)
        instance = self._new_instance({'sock_group': group.gr_name})
        instance.start()
        instance.stop()

        self.assertFileGID(instance.user_dir, gid)
        self.assertFileGID(instance.sock_file, gid)

    def test_invalid_sock_access(self):
        instance = self._new_instance({'sock_access': 'bogus'},
                                      expect_success=False)
        with self.assertRaises(pywatchman.SocketConnectError) as ctx:
            instance.start()
        self.assertEqual(ctx.exception.sockpath, instance.getSockPath())
        wanted = 'Expected config value sock_access to be an object'
        self.assertWaitFor(lambda: wanted in instance.getCLILogContents())

        instance = self._new_instance({'sock_access': {'group': 'oui'}},
                                      expect_success=False)
        with self.assertRaises(pywatchman.SocketConnectError) as ctx:
            instance.start()
        self.assertEqual(ctx.exception.sockpath, instance.getSockPath())
        wanted = 'Expected config value sock_access.group to be a boolean'
        self.assertWaitFor(lambda: wanted in instance.getCLILogContents())

    def test_default_sock_access(self):
        instance = self._new_instance({})
        instance.start()
        instance.stop()

        self.assertFileMode(instance.user_dir, 0o700 | stat.S_ISGID)
        self.assertFileMode(instance.sock_file, 0o600)

    def test_custom_sock_access_group(self):
        instance = self._new_instance({'sock_access': {'group': True}})
        instance.start()
        instance.stop()

        self.assertFileMode(instance.user_dir, 0o750 | stat.S_ISGID)
        self.assertFileMode(instance.sock_file, 0o660)

    def test_custom_sock_access_others(self):
        instance = self._new_instance({'sock_access': {'group': True,
                                                       'others': True}})
        instance.start()
        instance.stop()

        self.assertFileMode(instance.user_dir, 0o755 | stat.S_ISGID)
        self.assertFileMode(instance.sock_file, 0o666)

    def test_sock_access_upgrade(self):
        instance = self._new_instance({'sock_access': {'group': True,
                                                       'others': True}})
        os.makedirs(instance.user_dir)
        os.chmod(instance.user_dir, 0o700)
        instance.start()
        instance.stop()

        self.assertFileMode(instance.user_dir, 0o755 | stat.S_ISGID)
        self.assertFileMode(instance.sock_file, 0o666)

    def test_sock_access_downgrade(self):
        instance = self._new_instance({'sock_access': {'group': True}})
        os.makedirs(instance.user_dir)
        os.chmod(instance.user_dir, 0o755 | stat.S_ISGID)
        instance.start()
        instance.stop()

        self.assertFileMode(instance.user_dir, 0o750 | stat.S_ISGID)
        self.assertFileMode(instance.sock_file, 0o660)

    def test_sock_access_group_change(self):
        gid = self._get_custom_gid()
        group = grp.getgrgid(gid)
        instance = self._new_instance({'sock_group': group.gr_name})
        os.makedirs(instance.user_dir)
        # ensure that a different group is set
        os.chown(instance.user_dir, -1, os.getegid())
        instance.start()
        instance.stop()

        self.assertFileGID(instance.user_dir, gid)
        self.assertFileGID(instance.sock_file, gid)

    def assertFileMode(self, f, mode):
        st = os.lstat(f)
        self.assertEqual(stat.S_IMODE(st.st_mode), mode)

    def assertFileGID(self, f, gid):
        st = os.lstat(f)
        self.assertEqual(st.st_gid, gid)
