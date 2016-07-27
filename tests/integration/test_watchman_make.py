# vim: ts=4:sw=4:sts=4:et:
# Copyright 2012-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import WatchmanTestCase
import os
import os.path
import sys
import re
import subprocess
import signal
import time
import uuid

class TestWatchmanMake(WatchmanTestCase.WatchmanTestCase):

    def setUp(self):
        if sys.platform == 'win32':
            self.skipTest('Continuous integration testing only in Linux / MacOS')
        if sys.version_info >= (3, 0):
            self.skipTest('watchman-make is only compatible with Python 2')
        if self.transport != 'local' or self.encoding != 'bser':
            self.skipTest('Testing only with local transport and bser encoding')
        self.marker = 'target-' + str(uuid.uuid4())

    def _run_test(self, args, behavior):
        root = self.mkdtemp()
        self.touchRelative(root, '.watchmanconfig')
        fake_env = dict(os.environ)
        fake_env['PYTHONPATH'] = ':'.join(sys.path)
        fake_env['PATH'] = fake_env['PATH'] + ':' + os.getcwd()
        cmdline = [
            './python/bin/watchman-make',
            '-U', self.getClient().sockpath,
            '-s', '0.05',
            '--root', root,
            '--make', '/bin/echo']
        cmdline.extend(args)
        cmdline.extend(['-t', self.marker])
        process = subprocess.Popen(
            ' '.join(cmdline),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            shell=True,
            env=fake_env)
        time.sleep(1)
        behavior(root)
        process.send_signal(signal.SIGINT)
        return (process.stdout.read(), process.stderr.read())

    def test_noParams(self):
        def behavior(root):
            self.touchRelative(root, "foo")
            time.sleep(1)
        (stdout, stderr) = self._run_test([], behavior)
        print(stdout)
        print(stderr)
        self.assertTrue(
            "any file" in stderr,
            'A message about any file triggers the build')
        self.assertEquals(
            1, stdout.count(self.marker),
            'A single build triggered')

    def test_acceptParams(self):
        def behavior(root):
            self.touchRelative(root, 'afile.foo')
            self.touchRelative(root, 'anotherfile.bar')
            time.sleep(1)
        (stdout, stderr) = self._run_test(['-p', '**/*.foo'], behavior)
        self.assertTrue(
            "files matching" in stderr,
            'A message about only files matching the pattern trigger the build')
        self.assertEquals(
            1, stdout.count(self.marker),
            'Only matching file triggers build')

    def test_excludeParams(self):
        def behavior(root):
            self.touchRelative(root, 'afile.foo')
            self.touchRelative(root, 'anotherfile.bar')
            time.sleep(1)
        (stdout, stderr) = self._run_test(['-x', '**/*.bar'], behavior)
        self.assertTrue(
            "files not matching" in stderr,
            'A message about only files not matching pattern trigger the build')
        self.assertEquals(
            1, stdout.count(self.marker),
            'Only the file not matching pattern triggers the build')

    def test_acceptAndExcludeParams(self):
        def behavior(root):
            self.touchRelative(root, 'afile.foo')
            self.touchRelative(root, 'anotherfile.bar.foo')
            time.sleep(1)
            self.touchRelative(root, 'afile.bar.foo')
            self.touchRelative(root, 'anotherfile.foo')
            time.sleep(1)
        (stdout, stderr) = self._run_test(['-p', '**/*.foo', '-x', '**/*.bar.*'], behavior)
        self.assertTrue(
            "files matching" in stderr,
            'A message about only files matching the pattern trigger the build')
        self.assertTrue(
            "but not matching" in stderr,
            'A message about only files not matching the pattern trigger the build')
        self.assertEqual(
            2, stdout.count(self.marker),
            'Only files matching the required filters trigger the build')

