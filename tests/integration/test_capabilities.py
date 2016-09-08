# vim:ts=4:sw=4:et:
# Copyright 2012-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import WatchmanTestCase
import pywatchman
import pywatchman.capabilities


@WatchmanTestCase.expand_matrix
class TestCapabilities(WatchmanTestCase.WatchmanTestCase):

    def test_capabilities(self):
        client = self.getClient()
        res = client.query('version')
        self.assertFalse('error' in res, 'version with no args still works')

        res = client.query('version', {
            'optional': ['term-match', 'will-never-exist']})
        self.assertDictEqual(res['capabilities'], {
            'term-match': True,
            'will-never-exist': False})

        res = client.query('version', {
            'required': ['term-match'],
            'optional': ['will-never-exist']})
        self.assertDictEqual(res['capabilities'], {
            'term-match': True,
            'will-never-exist': False})
        self.assertFalse('error' in res, 'no error for missing optional')

        with self.assertRaisesRegexp(pywatchman.CommandError,
                                     'client required capability `will-never-exist` is not ' +
                                     'supported by this server'):
            client.query('version', {
                'required': ['term-match', 'will-never-exist']})

    def test_capabilityCheck(self):
        client = self.getClient()

        res = client.capabilityCheck(optional=[
                                     'term-match', 'will-never-exist'])
        self.assertDictEqual(res['capabilities'], {
            'term-match': True,
            'will-never-exist': False})

        res = client.capabilityCheck(
            required=['term-match'],
            optional=['will-never-exist'])
        self.assertDictEqual(res['capabilities'], {
            'term-match': True,
            'will-never-exist': False})

        with self.assertRaisesRegexp(pywatchman.CommandError,
                                     'client required capability `will-never-exist` is not ' +
                                     'supported by this server'):
            client.capabilityCheck(required=['term-match', 'will-never-exist'])

    def test_capabilitySynth(self):
        res = pywatchman.capabilities.synthesize({'version': '1.0'}, {
            'optional': ['will-never-exist'],
            'required': []})
        self.assertDictEqual(res, {
            'version': '1.0',
            'capabilities': {
                'will-never-exist': False}})

        res = pywatchman.capabilities.synthesize({'version': '1.0'}, {
            'required': ['will-never-exist'],
            'optional': []})
        self.assertDictEqual(res, {
            'version': '1.0',
            'error': 'client required capability `will-never-exist` ' +
                     'is not supported by this server',
            'capabilities': {
                'will-never-exist': False}})

        res = pywatchman.capabilities.synthesize({'version': '3.2'}, {
            'optional': ['relative_root'],
            'required': []})
        self.assertDictEqual(res, {
            'version': '3.2',
            'capabilities': {
                'relative_root': False}})
        res = pywatchman.capabilities.synthesize({'version': '3.3'}, {
            'optional': ['relative_root'],
            'required': []})
        self.assertDictEqual(res, {
            'version': '3.3',
            'capabilities': {
                'relative_root': True}})
