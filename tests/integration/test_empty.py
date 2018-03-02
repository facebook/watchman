# vim:ts=4:sw=4:et:
# Copyright 2012-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import WatchmanTestCase
import os


@WatchmanTestCase.expand_matrix
class TestEmpty(WatchmanTestCase.WatchmanTestCase):

    def test_empty(self):
        root = self.mkdtemp()
        self.touchRelative(root, 'empty')
        with open(os.path.join(root, 'notempty'), 'w') as f:
            f.write('foo')

        self.watchmanCommand('watch', root)
        results = self.watchmanCommand('query', root, {
            'expression': 'empty',
            'fields': ['name']})

        self.assertEqual(['empty'], results['files'])

        results = self.watchmanCommand('query', root, {
            'expression': 'exists',
            'fields': ['name']})

        self.assertFileListsEqual(['empty', 'notempty'], results['files'])

        clock = results['clock']
        os.unlink(os.path.join(root, 'empty'))

        self.assertFileList(root, files=['notempty'])

        results = self.watchmanCommand('query', root, {
            'expression': 'exists',
            'fields': ['name']})

        self.assertFileListsEqual(['notempty'], results['files'])

        # "files that don't exist" without a since term is absurd, so pass that in
        results = self.watchmanCommand('query', root, {
            'since': clock,
            'expression': ['not', 'exists'],
            'fields': ['name']})

        self.assertFileListsEqual(['empty'], results['files'])
