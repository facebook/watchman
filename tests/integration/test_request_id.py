# vim:ts=4:sw=4:et:
# Copyright 2018-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

# no unicode literals
from __future__ import absolute_import, division, print_function

import errno
import os
import re
import subprocess

import WatchmanInstance
import WatchmanTestCase


try:
    import unittest2 as unittest
except ImportError:
    import unittest


def is_hg_installed():
    with open(os.devnull, "wb") as devnull:
        try:
            env = os.environ.copy()
            env["HGPLAIN"] = "1"
            env["WATCHMAN_SOCK"] = WatchmanInstance.getSharedInstance().getSockPath()

            exit_code = subprocess.call(
                ["hg", "--version"], stdout=devnull, stderr=devnull, env=env
            )
            return exit_code == 0
        except OSError as e:
            if e.errno == errno.ENOENT:
                return False
            raise


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

    def skipIfNoHgRequestIdSupport(self):
        root = self.mkdtemp()
        request_id = "bf8a47014bd1b66103a8ab0aece4be7ada871660"

        env = os.environ.copy()
        env["HGPLAIN"] = "1"
        env["HGREQUESTID"] = request_id
        env["WATCHMAN_SOCK"] = WatchmanInstance.getSharedInstance().getSockPath()

        subprocess.call(["hg", "init"], env=env, cwd=root)
        subprocess.call(["hg", "log"], env=env, cwd=root)

        try:
            with open(os.path.join(root, ".hg/blackbox.log")) as f:
                if request_id in f.read():
                    return
        except IOError:
            pass

        self.skipTest("HGREQUESTID is not supported")

    @unittest.skipIf(not is_hg_installed(), "Hg not installed")
    def test_scmHgRequestId(self):
        self.skipIfNoHgRequestIdSupport()

        root = self.mkdtemp()

        # In this test, the repo does not necessarily need fsmonitor enabled,
        # since watchman calls HGREQUESTID=... hg status and that would also
        # have request_id logged without fsmonitor.
        env = os.environ.copy()
        env["HGPLAIN"] = "1"
        env["WATCHMAN_SOCK"] = WatchmanInstance.getSharedInstance().getSockPath()
        subprocess.call(["hg", "init"], env=env, cwd=root)
        subprocess.call(
            [
                "hg",
                "commit",
                "-mempty",
                "-utest",
                "-d0 0",
                "--config=ui.allowemptycommit=1",
            ],
            env=env,
            cwd=root,
        )
        commit_hash = subprocess.check_output(
            ["hg", "log", "-r.", "-T{node}"], env=env, cwd=root
        ).decode("utf-8")

        # Must watch the directory after it's an HG repo to perform scm-aware
        # queries.
        self.watchmanCommand("watch", root)
        request_id = "4c05a798ea1acc7c97b75e61fec5f640d90f8209"

        params = {
            "fields": ["name"],
            "request_id": request_id,
            "since": {"scm": {"mergebase-with": commit_hash}},
        }
        self.watchmanCommand("query", root, params)

        blackbox_path = os.path.join(root, ".hg", "blackbox.log")

        def try_read_blackbox():
            try:
                with open(blackbox_path) as f:
                    return f.read()
            except IOError:
                return ""

        self.assertWaitFor(
            lambda: request_id in try_read_blackbox(),
            message="request_id passed to and logged by hg",
        )
