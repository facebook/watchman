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
import sys
import tempfile
import time
try:
    import unittest2 as unittest
except ImportError:
    import unittest

import pywatchman

@unittest.skipIf(os.name == 'nt' or \
                 sys.platform == 'darwin' or \
                 os.geteuid() == 0, "win or root or bad ldap")
class TestSockPerms(unittest.TestCase):
    def _new_instance(self, config, expect_success=True):
        if expect_success:
            start_timeout = 10
        else:
            # If the instance is going to fail anyway then there's no point
            # waiting so long
            start_timeout = 0.5
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

    def _get_non_member_group(self):
        """Get a group tuple that this user is not a member of."""
        user_groups = set(os.getgroups())
        for group in grp.getgrall():
            if group.gr_gid not in user_groups:
                return group
        self.skipTest('no usable groups found')

    def waitFor(self, cond, timeout=10):
        deadline = time.time() + timeout
        res = None
        while time.time() < deadline:
            try:
                res = cond()
                if res:
                    return [True, res]
            except Exception:
                pass
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

    def test_user_not_in_sock_group(self):
        group = self._get_non_member_group()
        instance = self._new_instance({'sock_group': group.gr_name},
                                      expect_success=False)
        with self.assertRaises(pywatchman.SocketConnectError) as ctx:
            instance.start()
        self.assertEqual(ctx.exception.sockpath, instance.getSockPath())
        wanted = "setting up group '%s' failed" % group.gr_name
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

    def test_user_previously_in_sock_group(self):
        """This tests the case where a user was previously in sock_group
        (so Watchman created the directory with that group), but no longer is
        (so the socket is created with a different group)."""
        # Since it's hard to drop a group from a process without being
        # superuser, fake it. Use a private testing-only config option to set
        # up separate groups for the directory and the file.
        gid = self._get_custom_gid()
        group = grp.getgrgid(gid)
        non_member_group = self._get_non_member_group()
        # Need to wait for the server to come up here, can't use
        # expect_success=False.
        instance = self._new_instance(
            {'sock_group': group.gr_name,
             '__sock_file_group': non_member_group.gr_name})
        with self.assertRaises(pywatchman.SocketConnectError):
            instance.start()

        wanted = ("for socket '%s', gid %d doesn't match expected gid %d "
                  "(group name %s)." %
                  (instance.getSockPath(), gid, non_member_group.gr_gid,
                   non_member_group.gr_name))
        self.assertWaitFor(lambda: wanted in instance.getServerLogContents())

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
