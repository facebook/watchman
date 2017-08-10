# vim:ts=4:sw=4:et:
# Copyright 2017-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import WatchmanEdenTestCase
import os
import pywatchman


class TestEdenWatchRoot(WatchmanEdenTestCase.WatchmanEdenTestCase):
    def test_eden_watch_root(self):
        def populate(repo):
            repo.write_file('adir/file', 'foo!\n')
            repo.commit('initial commit.')

        root = self.makeEdenMount(populate)

        with self.assertRaises(pywatchman.WatchmanError) as ctx:
            self.watchmanCommand('watch', os.path.join(root, 'adir'))
        self.assertRegexpMatches(
            str(ctx.exception),
            ("unable to resolve root .*: eden: you may only watch " +
             "from the root of an eden mount point."))
