# vim:ts=4:sw=4:et:
# Copyright 2016-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import os
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

    def test_too_open_user_dir(self):
        instance = self._new_instance({}, expect_success=False)
        os.makedirs(instance.user_dir)
        os.chmod(instance.user_dir, 0o777)
        with self.assertRaises(pywatchman.SocketConnectError) as ctx:
            instance.start()
        self.assertEqual(ctx.exception.sockpath, instance.getSockPath())
        self.assertIn('the permissions on %s allow others to write to it'
                      % instance.user_dir, instance.getCLILogContents())
