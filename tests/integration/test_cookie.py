# vim:ts=4:sw=4:et:
# Copyright 2018-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

# no unicode literals
from __future__ import absolute_import, division, print_function

import os
import socket

import pywatchman
import WatchmanTestCase


@WatchmanTestCase.expand_matrix
class TestCookie(WatchmanTestCase.WatchmanTestCase):
    def test_delete_cookie_dir(self):
        root = self.mkdtemp()
        cookie_dir = os.path.join(root, ".hg")
        os.mkdir(cookie_dir)
        self.touchRelative(root, "foo")

        self.watchmanCommand("watch-project", root)
        self.assertFileList(root, files=["foo", ".hg"])

        os.rmdir(cookie_dir)
        self.assertFileList(root, files=["foo"])
        os.unlink(os.path.join(root, "foo"))
        self.assertFileList(root, files=[])
        os.rmdir(root)
        with self.assertRaises(pywatchman.WatchmanError) as ctx:
            result = self.assertFileList(root, files=[])
            print("Should not have gotten here, but the result was:", result)

        reason = str(ctx.exception)
        self.assertTrue(
            ("No such file" in reason)
            or ("root dir was removed" in reason)
            or ("The system cannot find the file specified" in reason)
            or ("unable to resolve root" in reason),
            msg=reason,
        )

    def test_other_cookies(self):
        root = self.mkdtemp()
        cookie_dir = os.path.join(root, ".git")
        os.mkdir(cookie_dir)
        self.watchmanCommand("watch", root)

        host = socket.gethostname()
        pid = self.watchmanCommand("get-pid")["pid"]

        self.assertFileList(root, files=[".git"])
        os.mkdir(os.path.join(root, "foo"))

        # Same process, same watch
        self.touchRelative(root, ".git/.watchman-cookie-%s-%d-1000000" % (host, pid))

        cookies = [
            # Same process, different watch root
            "foo/.watchman-cookie-%s-%d-100000" % (host, pid),
            # Same process, root dir instead of VCS dir
            ".watchman-cookie-%s-%d-100000" % (host, pid),
            # Different process, same watch root
            ".git/.watchman-cookie-%s-1-100000" % host,
            # Different process, root dir instead of VCS dir
            ".watchman-cookie-%s-1-100000" % host,
            # Different process, different watch root
            "foo/.watchman-cookie-%s-1-100000" % host,
        ]

        for cookie in cookies:
            self.touchRelative(root, cookie)

        self.assertFileList(root, files=["foo", ".git"] + cookies)
