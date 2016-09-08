# vim:ts=4:sw=4:et:
# Copyright 2016-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals
import json
import os
import tempfile

try:
    import unittest2 as unittest
except ImportError:
    import unittest

import WatchmanInstance

WATCHMAN_SRC_DIR = os.environ.get('WATCHMAN_SRC_DIR', os.getcwd())
THIS_DIR = os.path.join(WATCHMAN_SRC_DIR, 'tests', 'integration')

@unittest.skipIf(os.name == 'nt', "not supported on windows")
class TestSiteSpawn(unittest.TestCase):
    def test_failingSpawner(self):
      config = {
          'spawn_watchman_service': os.path.join(THIS_DIR, 'site_spawn_fail.py')
      }

      inst = WatchmanInstance.Instance(config=config)
      stdout, stderr = inst.commandViaCLI(['version'])

      self.assertEqual(b'', stdout)
      self.assertRegexpMatches(stderr.decode('ascii'),
            'site_spawn_fail.py: exited with status 1')
      with open(inst.log_file_name, 'r') as f:
          self.assertEqual('failed to start\n', f.read())

    def test_spawner(self):
      config = {
          'spawn_watchman_service': os.path.join(THIS_DIR, 'site_spawn.py')
      }

      inst = WatchmanInstance.Instance(config=config)
      stdout, stderr = inst.commandViaCLI(['version'])

      parsed = json.loads(stdout.decode('ascii'))
      self.assertTrue('version' in parsed)

      # Shut down that process, as we have no automatic way to deal with it
      inst.commandViaCLI(['--no-spawn', '--no-local', 'shutdown-server'])
