# Copyright 2015-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import WatchmanTestCase
import os
import subprocess
import sys
import WatchmanInstance


@WatchmanTestCase.expand_matrix
class TestWatchmanWait(WatchmanTestCase.WatchmanTestCase):
    def requiresPersistentSession(self):
        return True

    def spawnWatchmanWait(self, cmdArgs):
        wait_script = os.environ.get('WATCHMAN_WAIT_PATH')
        if wait_script:
            args = [
                wait_script,
            ]
        else:
            args = [
                sys.executable,
                os.path.join(
                    os.environ['WATCHMAN_PYTHON_BIN'],
                    'watchman-wait'),
            ]
        args.extend(cmdArgs)

        env = os.environ.copy()
        sock_path = WatchmanInstance.getSharedInstance().getSockPath()
        env["WATCHMAN_SOCK"] = sock_path
        env["PYTHONPATH"] = env["PYWATCHMAN_PATH"]
        return subprocess.Popen(args,
                                env=env,
                                stdin=None,
                                stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE)

    def assertWaitedFileList(self, stdout, expected):
        stdout = stdout.decode('utf-8').rstrip()
        files = [f.rstrip() for f in stdout.split('\n')]
        files = set(self.normFileList(files))
        self.assertFileListContains(files, expected)

    def test_wait(self):
        root = self.mkdtemp()
        self.touchRelative(root, 'foo')
        a_dir = os.path.join(root, 'a')
        os.mkdir(a_dir)
        self.touchRelative(a_dir, 'foo')

        wmwait = self.spawnWatchmanWait(['--relative', root,
                                         '-m', '8', '-t', '3', root])

        # watchman-wait will establish the watch, so we need to wait for that
        # to complete before we start making the changes that we want to
        # observe through it.
        self.assertWaitFor(lambda: self.rootIsWatched(root))

        self.touchRelative(root, 'bar')
        self.removeRelative(root, 'foo')
        self.touchRelative(a_dir, 'bar')
        self.removeRelative(a_dir, 'foo')

        b_dir = os.path.join(root, 'b')
        os.mkdir(b_dir)
        self.touchRelative(b_dir, 'foo')

        (stdout, stderr) = wmwait.communicate()
        self.assertWaitedFileList(stdout, [
            'a/bar',
            'a/foo',
            'b/foo',
            'bar',
            'foo'])

    def test_rel_root(self):
        root = self.mkdtemp()

        a_dir = os.path.join(root, 'a')
        os.mkdir(a_dir)
        b_dir = os.path.join(root, 'b')
        os.mkdir(b_dir)

        wmwait = self.spawnWatchmanWait(['--relative', b_dir,
                                         '-m', '8', '-t', '6', a_dir, b_dir])

        # watchman-wait will establish the watches, so we need to wait for that
        # to complete before we start making the changes that we want to
        # observe through it.
        self.assertWaitFor(lambda: self.rootIsWatched(b_dir))
        self.assertWaitFor(lambda: self.rootIsWatched(a_dir))

        self.touchRelative(a_dir, 'afoo')
        self.touchRelative(b_dir, 'bfoo')

        a_sub_dir = os.path.join(a_dir, 'asub')
        os.mkdir(a_sub_dir)
        b_sub_dir = os.path.join(b_dir, 'bsub')
        os.mkdir(b_sub_dir)

        (stdout, stderr) = wmwait.communicate()
        self.assertWaitedFileList(stdout, [
            '../a/afoo',
            '../a/asub',
            'bfoo',
            'bsub'])
