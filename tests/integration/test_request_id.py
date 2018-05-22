# vim:ts=4:sw=4:et:
# Copyright 2018-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

# no unicode literals
from __future__ import absolute_import, division, print_function

import re

import WatchmanTestCase


@WatchmanTestCase.expand_matrix
class TestRequestId(WatchmanTestCase.WatchmanTestCase):

    def test_queryRequestId(self):
        root = self.mkdtemp()
        self.watchmanCommand("watch", root)
        self.watchmanCommand("log-level", "debug")
        self.touchRelative(root, "111")

        request_id = "f13bd3bc02c27afe2413932c6fa6c4942b0574b3"
        params = {"since": "c:0:0", "request_id": request_id}
        self.watchmanCommand("query", root, params)
        pat = re.compile(".* \[client=.*\] request_id = %s" % request_id)

        self.assertWaitFor(
            lambda: any(pat.match(l) for l in self.getServerLogContents()),
            message="request_id logged",
        )
