# vim:ts=4:sw=4:et:
# Copyright 2017-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

# no unicode literals
from __future__ import absolute_import, division, print_function

import hashlib
import json
import os

import pywatchman
import WatchmanTestCase


@WatchmanTestCase.expand_matrix
class TestContentHash(WatchmanTestCase.WatchmanTestCase):
    def write_file_and_hash(self, filename, content):
        content = content.encode("utf-8")
        with open(filename, "wb") as f:
            f.write(content)

        sha = hashlib.sha1()
        sha.update(content)
        return sha.hexdigest()

    def test_contentHash(self):
        root = self.mkdtemp()

        expect_hex = self.write_file_and_hash(os.path.join(root, "foo"), "hello\n")
        self.watchmanCommand("watch", root)
        self.assertFileList(root, ["foo"])

        stats = self.watchmanCommand("debug-contenthash", root)
        self.assertEqual(stats["size"], 0)

        res = self.watchmanCommand(
            "query",
            root,
            {"expression": ["name", "foo"], "fields": ["name", "content.sha1hex"]},
        )
        self.assertEqual(expect_hex, res["files"][0]["content.sha1hex"])

        stats = self.watchmanCommand("debug-contenthash", root)
        self.assertEqual(stats["size"], 1)
        self.assertEqual(stats["cacheHit"], 0)
        self.assertEqual(stats["cacheStore"], 1)

        # repeated query also works
        res = self.watchmanCommand(
            "query",
            root,
            {"expression": ["name", "foo"], "fields": ["name", "content.sha1hex"]},
        )
        self.assertEqual(expect_hex, res["files"][0]["content.sha1hex"])

        stats = self.watchmanCommand("debug-contenthash", root)
        self.assertEqual(stats["size"], 1)
        self.assertEqual(stats["cacheHit"], 1)
        self.assertEqual(stats["cacheStore"], 1)

        # change the content and expect to see that reflected
        # in the subsequent query.  We query two at the same time to ensure that
        # exercise the batch fetching code and that we get sane results for both
        # entries.
        expect_hex = self.write_file_and_hash(os.path.join(root, "foo"), "goodbye\n")
        expect_bar_hex = self.write_file_and_hash(
            os.path.join(root, "bar"), "different\n"
        )

        res = self.watchmanCommand(
            "query",
            root,
            {"paths": ["foo", "bar"], "fields": ["name", "content.sha1hex"]},
        )
        self.assertEqual(
            [
                {"name": "bar", "content.sha1hex": expect_bar_hex},
                {"name": "foo", "content.sha1hex": expect_hex},
            ],
            sorted(res["files"], key=lambda k: k["name"]),
        )

        stats = self.watchmanCommand("debug-contenthash", root)
        self.assertEqual(stats["size"], 3)
        self.assertEqual(stats["cacheHit"], 1)
        self.assertEqual(stats["cacheMiss"], 3)
        self.assertEqual(stats["cacheStore"], 3)

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

    def test_contentHashWarming(self):
        root = self.mkdtemp()

        expect_hex = self.write_file_and_hash(os.path.join(root, "foo"), "hello\n")
        with open(os.path.join(root, ".watchmanconfig"), "w") as f:
            f.write(json.dumps({"content_hash_warming": True}))

        self.watchmanCommand("watch", root)
        self.assertFileList(root, [".watchmanconfig", "foo"])

        def cachePopulate():
            return self.watchmanCommand("debug-contenthash", root)["size"] == 2

        self.waitFor(cachePopulate)
        stats = self.watchmanCommand("debug-contenthash", root)
        self.assertEqual(stats["size"], 2)
        self.assertEqual(stats["cacheHit"], 0)
        self.assertEqual(stats["cacheMiss"], 2)
        self.assertEqual(stats["cacheStore"], 2)
        self.assertEqual(stats["cacheLoad"], 2)

        res = self.watchmanCommand(
            "query",
            root,
            {"expression": ["name", "foo"], "fields": ["name", "content.sha1hex"]},
        )
        self.assertEqual(expect_hex, res["files"][0]["content.sha1hex"])

    def test_cacheLimit(self):
        root = self.mkdtemp()

        expect_hex = self.write_file_and_hash(os.path.join(root, "foo"), "hello\n")
        expect_bar_hex = self.write_file_and_hash(
            os.path.join(root, "bar"), "different\n"
        )

        with open(os.path.join(root, ".watchmanconfig"), "w") as f:
            f.write(json.dumps({"content_hash_max_items": 1}))

        self.watchmanCommand("watch", root)
        self.assertFileList(root, [".watchmanconfig", "foo", "bar"])

        with self.assertRaises(pywatchman.WatchmanError) as ctx:
            res = self.watchmanCommand(
                "query",
                root,
                {"paths": ["foo", "bar"], "fields": ["name", "content.sha1hex"]},
            )

            # TODO: we expect the query to yield these results, but at the
            # time of writing, a limitation in the LRUCache code prevents
            # us from generating the results
            self.assertEqual(
                [
                    {"name": "bar", "content.sha1hex": expect_bar_hex},
                    {"name": "foo", "content.sha1hex": expect_hex},
                ],
                sorted(res["files"], key=lambda k: k["name"]),
            )
        self.assertIn("pending cache", str(ctx.exception))
