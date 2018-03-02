# vim:ts=4:sw=4:et:
# Copyright 2012-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import WatchmanTestCase
import os
import subprocess


try:
    from shlex import quote as shellquote
except ImportError:
    from pipes import quote as shellquote


@WatchmanTestCase.expand_matrix
class TestFishy(WatchmanTestCase.WatchmanTestCase):
    def checkOSApplicability(self):
        if os.name == 'nt':
            self.skipTest('non admin symlinks and unix userland not available')

    def test_fishy(self):
        root = self.mkdtemp()

        os.mkdir(os.path.join(root, 'foo'))
        self.touchRelative(root, 'foo', 'a')

        self.watchmanCommand('watch', root)
        base = self.watchmanCommand('find', root, '.')
        clock = base['clock']

        self.suspendWatchman()
        # Explicitly using the shell to run these commands
        # as the original test case wasn't able to reproduce
        # the problem with whatever sequence and timing of
        # operations was produced by the original php test
        subprocess.check_call("cd %s && mv foo bar && ln -s bar foo" %
            shellquote(root),
            shell=True)

        self.resumeWatchman()
        self.assertFileList(root, files=['bar', 'bar/a', 'foo'], cursor=clock)

    def test_more_moves(self):
        root = self.mkdtemp()
        self.watchmanCommand('watch', root)
        base = self.watchmanCommand('find', root, '.')
        clock = base['clock']

        self.suspendWatchman()
        # Explicitly using the shell to run these commands
        # as the original test case wasn't able to reproduce
        # the problem with whatever sequence and timing of
        # operations was produced by the original php test
        subprocess.check_call(
            "cd %s && touch a && mkdir d1 d2 && mv d1 d2 && mv d2/d1 . && mv a d1" %
            shellquote(root), shell=True)
        self.resumeWatchman()
        self.assertFileList(root, files=['d1', 'd1/a', 'd2'], cursor=clock)

    def test_even_more_moves(self):
        root = self.mkdtemp()
        self.watchmanCommand('watch', root)
        base = self.watchmanCommand('find', root, '.')
        clock = base['clock']

        self.suspendWatchman()
        # Explicitly using the shell to run these commands
        # as the original test case wasn't able to reproduce
        # the problem with whatever sequence and timing of
        # operations was produced by the original php test
        subprocess.check_call((
            "cd %s && "
            "mkdir d1 d2 && "
            "touch d1/a && "
            "mkdir d3 && "
            "mv d1 d2 d3 && "
            "mv d3/* . && "
            "mv d1 d2 d3 && "
            "mv d3/* . && "
            "mv d1/a d2") %
            shellquote(root), shell=True)
        self.resumeWatchman()
        self.assertFileList(root, files=['d1', 'd2', 'd2/a', 'd3'], cursor=clock)
