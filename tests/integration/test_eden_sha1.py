# vim:ts=4:sw=4:et:
# Copyright 2016-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

# no unicode literals
from __future__ import absolute_import, division, print_function

import hashlib
import os

import WatchmanEdenTestCase


class TestEdenSha1(WatchmanEdenTestCase.WatchmanEdenTestCase):

    def write_file_and_hash(self, filename, content):
        content = content.encode("utf-8")
        with open(filename, "wb") as f:
            f.write(content)

        sha = hashlib.sha1()
        sha.update(content)
        return sha.hexdigest()

    def test_eden_sha1(self):

        def populate(repo):
            repo.write_file("hello", "hola\n")
            repo.write_file("adir/file", "foo!\n")
            repo.write_file("bdir/test.sh", "#!/bin/bash\necho test\n", mode=0o755)
            repo.write_file("bdir/noexec.sh", "#!/bin/bash\necho test\n")
            repo.symlink("slink", "hello")
            repo.commit("initial commit.")

        root = self.makeEdenMount(populate)

        res = self.watchmanCommand("watch", root)
        self.assertEqual("eden", res["watcher"])

        expect_hex = self.write_file_and_hash(os.path.join(root, "foo"), "hello\n")

        res = self.watchmanCommand(
            "query",
            root,
            {"expression": ["name", "foo"], "fields": ["name", "content.sha1hex"]},
        )
        self.assertEqual(expect_hex, res["files"][0]["content.sha1hex"])

        # repeated query also works
        res = self.watchmanCommand(
            "query",
            root,
            {"expression": ["name", "foo"], "fields": ["name", "content.sha1hex"]},
        )
        self.assertEqual(expect_hex, res["files"][0]["content.sha1hex"])

        # change the content and expect to see that reflected
        # in the subsequent query
        expect_hex = self.write_file_and_hash(os.path.join(root, "foo"), "goodbye\n")

        res = self.watchmanCommand(
            "query",
            root,
            {"expression": ["name", "foo"], "fields": ["name", "content.sha1hex"]},
        )
        self.assertEqual(expect_hex, res["files"][0]["content.sha1hex"])

        # directories have no content hash
        os.mkdir(os.path.join(root, "dir"))
        res = self.watchmanCommand(
            "query",
            root,
            {"expression": ["name", "dir"], "fields": ["name", "content.sha1hex"]},
        )
        self.assertEqual(None, res["files"][0]["content.sha1hex"])

        # removed files have no content hash
        os.unlink(os.path.join(root, "foo"))
        res = self.watchmanCommand(
            "query",
            root,
            {
                "expression": ["name", "foo"],
                # need to a since query so that the removed files
                # show up in the results
                "since": res["clock"],
                "fields": ["name", "content.sha1hex"],
            },
        )
        self.assertEqual(None, res["files"][0]["content.sha1hex"])
