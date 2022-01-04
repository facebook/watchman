# vim:ts=4:sw=4:et:
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# no unicode literals
from __future__ import absolute_import, division, print_function

import os

import WatchmanTestCase


@WatchmanTestCase.expand_matrix
class TestWatchmanConfigValid(WatchmanTestCase.WatchmanTestCase):
    def test_trailing_comma(self):
        root = self.mkdtemp()
        with open(os.path.join(root, ".watchmanconfig"), "w") as f:
            f.write('{"ignore_dirs":["foo",],}')

        with self.assertRaises(Exception) as ctx:
            self.watchmanCommand("watch", root)
        self.assertIn("failed to parse json", str(ctx.exception))
