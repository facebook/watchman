# vim:ts=4:sw=4:et:
# Copyright 2017-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import WatchmanTestCase
import hashlib
import json
import os


@WatchmanTestCase.expand_matrix
class TestContentHash(WatchmanTestCase.WatchmanTestCase):
    def write_file_and_hash(self, filename, content):
        content = content.encode("utf-8")
        with open(filename, 'wb') as f:
            f.write(content)

        sha = hashlib.sha1()
        sha.update(content)
        return sha.hexdigest()

    def test_contentHash(self):
        root = self.mkdtemp()

        expect_hex = self.write_file_and_hash(
            os.path.join(root, 'foo'), "hello\n")
        self.watchmanCommand('watch', root)
        self.assertFileList(root, ['foo'])

        stats = self.watchmanCommand('debug-contenthash', root)
        self.assertEqual(stats['size'], 0)

        res = self.watchmanCommand('query', root, {
            'expression': ["name", "foo"],
            'fields': ['name', 'content.sha1hex']})
        self.assertEqual(expect_hex,
                         res['files'][0]['content.sha1hex'])

        stats = self.watchmanCommand('debug-contenthash', root)
        self.assertEqual(stats['size'], 1)
        self.assertEqual(stats['cacheHit'], 0)
        self.assertEqual(stats['cacheStore'], 1)

        # repeated query also works
        res = self.watchmanCommand('query', root, {
            'expression': ["name", "foo"],
            'fields': ['name', 'content.sha1hex']})
        self.assertEqual(expect_hex,
                         res['files'][0]['content.sha1hex'])

        stats = self.watchmanCommand('debug-contenthash', root)
        self.assertEqual(stats['size'], 1)
        self.assertEqual(stats['cacheHit'], 1)
        self.assertEqual(stats['cacheStore'], 1)

        # change the content and expect to see that reflected
        # in the subsequent query
        expect_hex = self.write_file_and_hash(
            os.path.join(root, 'foo'), "goodbye\n")

        res = self.watchmanCommand('query', root, {
            'expression': ["name", "foo"],
            'fields': ['name', 'content.sha1hex']})
        self.assertEqual(expect_hex,
                         res['files'][0]['content.sha1hex'])

        stats = self.watchmanCommand('debug-contenthash', root)
        self.assertEqual(stats['size'], 2)
        self.assertEqual(stats['cacheHit'], 1)
        self.assertEqual(stats['cacheMiss'], 2)
        self.assertEqual(stats['cacheStore'], 2)

        # directories have no content hash
        os.mkdir(os.path.join(root, "dir"))
        res = self.watchmanCommand('query', root, {
            'expression': ["name", "dir"],
            'fields': ['name', 'content.sha1hex']})
        self.assertEqual(None,
                         res['files'][0]['content.sha1hex'])

        # removed files have no content hash
        os.unlink(os.path.join(root, 'foo'))
        res = self.watchmanCommand('query', root, {
            'expression': ["name", "foo"],
            # need to a since query so that the removed files
            # show up in the results
            'since': res['clock'],
            'fields': ['name', 'content.sha1hex']})
        self.assertEqual(None,
                         res['files'][0]['content.sha1hex'])

    def test_contentHashWarming(self):
        root = self.mkdtemp()

        expect_hex = self.write_file_and_hash(
            os.path.join(root, 'foo'), "hello\n")
        with open(os.path.join(root, '.watchmanconfig'), 'w') as f:
            f.write(json.dumps({'content_hash_warming': True,}))

        self.watchmanCommand('watch', root)
        self.assertFileList(root, ['.watchmanconfig', 'foo'])

        def cachePopulate():
            return self.watchmanCommand(
                'debug-contenthash', root)['size'] == 2

        self.waitFor(cachePopulate)
        stats = self.watchmanCommand('debug-contenthash', root)
        self.assertEqual(stats['size'], 2)
        self.assertEqual(stats['cacheHit'], 0)
        self.assertEqual(stats['cacheMiss'], 2)
        self.assertEqual(stats['cacheStore'], 2)
        self.assertEqual(stats['cacheLoad'], 2)

        res = self.watchmanCommand('query', root, {
            'expression': ["name", "foo"],
            'fields': ['name', 'content.sha1hex']})
        self.assertEqual(expect_hex,
                         res['files'][0]['content.sha1hex'])
