# vim:ts=4:sw=4:et:
# Copyright 2012-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import errno
try:
    import unittest2 as unittest
except ImportError:
    import unittest
import pywatchman
import time
import tempfile
import os.path
import os
import WatchmanInstance
import copy
import sys

if pywatchman.compat.PYTHON3:
    STRING_TYPES = (str, bytes)
else:
    STRING_TYPES = (str, unicode)

def norm_path(name):
    return os.path.normcase(os.path.normpath(name))

# TODO: This normalization will not be needed once we have full unicode support
# in place as per
# https://github.com/facebook/watchman/wiki/Better-Unicode-handling-plan
def conv_path_to_bytes(path):
    if isinstance(path, pywatchman.compat.UNICODE):
        return path.encode('utf8')
    else:
        return path

if os.name == 'nt':
    # monkey patch to hopefully minimize test flakiness
    def wrap_with_backoff(fn):
        def wrapper(*args, **kwargs):
            delay = 0.01
            attempts = 10
            while True:
                try:
                    return fn(*args, **kwargs)
                except WindowsError as e:
                    if attempts == 0:
                        raise
                    # WindowsError: [Error 32] The process cannot access the
                    # file because it is being used by another process.
                    # Error 5: Access is denied.
                    if e.winerror not in (5, 32):
                        raise

                attempts = attempts - 1
                time.sleep(delay)
                delay = delay * 2

        return wrapper

    for name in ['rename', 'unlink', 'remove', 'rmdir']:
        setattr(os, name, wrap_with_backoff(getattr(os, name)))

class WatchmanTestCase(unittest.TestCase):

    def requiresPersistentSession(self):
        return False

    def checkPersistentSession(self):
        if self.requiresPersistentSession() and self.transport == 'cli':
            self.skipTest('need persistent session')

    def setUp(self):
        self.checkPersistentSession()

    def getClient(self, inst=None):
        if inst or not hasattr(self, 'client'):
            client = pywatchman.client(
                # ASAN-enabled builds can be slower enough that we hit timeouts
                # with the default of 1 second
                timeout=3.0,
                transport=self.transport,
                sendEncoding=self.encoding,
                recvEncoding=self.encoding,
                sockpath=(inst or WatchmanInstance.getSharedInstance()).
                         getSockPath())
            if not inst:
                # only cache the client if it points to the shared instance
                self.client = client
            return client
        return self.client

    def __logTestInfo(self, test, msg):
        if hasattr(self, 'client'):
            try:
                self.getClient().query('log', 'debug',
                                       'TEST: %s %s\n\n' % (test, msg))
            except Exception as e:
                pass

    def normPath(self, name):
        return norm_path(name)

    def mkdtemp(self, **kwargs):
        return self.normPath(tempfile.mkdtemp(dir=self.tempdir, **kwargs))

    def mktemp(self, prefix=''):
        f, name = tempfile.mkstemp(prefix=prefix, dir=self.tempdir)
        os.close(f)
        return name

    def run(self, result):
        if result is None:
            raise Exception('MUST be a runtests.py:Result instance')
        # Arrange for any temporary stuff we create to go under
        # our global tempdir and put it in a dir named for the test
        id = '%s.%s.%s' % (self.id(), self.transport, self.encoding)
        try:
            self.tempdir = os.path.join(tempfile.tempdir, id)
            os.mkdir(self.tempdir)
            self.__logTestInfo(id, 'BEGIN')
            result.setFlavour(self.transport, self.encoding)
            super(WatchmanTestCase, self).run(result)
        finally:
            try:
                self.watchmanCommand('log-level', 'off')
                self.getClient().getLog(remove=True)
            except:
                pass
            self.__logTestInfo(id, 'END')
            self.__clearWatches()
            if hasattr(self, 'client'):
                self.client.close()
                delattr(self, 'client')

        return result

    def expandConfigurations(self):
        tests = []
        for transport, encoding in [('local', 'bser'),
                                    ('local', 'experimental-bser-v2'),
                                    ('local', 'json'),
                                    ('cli', 'json')]:
            test = copy.copy(self)
            test.setConfiguration(transport, encoding)
            tests.append(test)
        return tests

    def setConfiguration(self, transport, encoding):
        self.transport = transport
        self.encoding = encoding


    def touch(self, fname, times=None):
        try:
            os.utime(fname, times)
        except OSError as e:
            if e.errno == errno.ENOENT:
                with open(fname, 'a'):
                    os.utime(fname, times)
            else:
                raise

    def touchRelative(self, base, *fname):
        fname = os.path.join(base, *fname)
        self.touch(fname, None)

    def __clearWatches(self):
        if hasattr(self, 'client'):
            try:
                self.client.subs = {}
                self.client.sub_by_root = {}
                self.watchmanCommand('watch-del-all')
            except Exception as e:
                pass

    def __del__(self):
        self.__clearWatches()

    def watchmanCommand(self, *args):
        return self.getClient().query(*args)

    def decodeBSERUTF8(self, s, surrogateescape=False):
        if pywatchman.compat.PYTHON3 and \
                (self.encoding == 'experimental-bser-v2' or \
                 self.encoding == 'bser'):
            if surrogateescape:
                errors = 'surrogateescape'
            else:
                errors = 'strict'
            return s.decode('utf-8', errors)
        return s

    def assertEqualUTF8Strings(self, expected, actual, surrogateescape=False):
        '''assert that actual (possibly a UTF-8 encoded byte string) is equal
        to expected (a Unicode string in Python 3)'''
        self.assertEqual(expected, self.decodeBSERUTF8(actual))

    # Continually invoke `cond` until it returns true or timeout
    # is reached.  Returns a tuple of [bool, result] where the
    # first element of the tuple indicates success/failure and
    # the second element is the return value from the condition
    def waitFor(self, cond, timeout=10):
        deadline = time.time() + timeout
        res = None
        while time.time() < deadline:
            res = cond()
            if res:
                return [True, res]
            time.sleep(0.03)
        return [False, res]

    def assertWaitFor(self, cond, timeout=10, message=None):
        status, res = self.waitFor(cond, timeout)
        if status:
            return res
        if message is None:
            message = "%s was not met in %s seconds: %s" % (cond, timeout, res)
        self.fail(message)

    def getFileList(self, root, cursor=None, relativeRoot=None):
        expr = {
            "expression": ["exists"],
            "fields": ["name"],
        }
        if cursor:
            expr['since'] = cursor
        if relativeRoot:
            expr['relative_root'] = relativeRoot
        res = self.watchmanCommand('query', root, expr)
        files = self.normWatchmanFileList(res['files'])
        self.last_file_list = files
        return files
    def waitForSync(self, root):
        """ ensure that watchman has observed any pending file changes
            This is most useful after mutating the filesystem and before
            attempting to perform a since query
        """
        self.watchmanCommand('query', root, {
            'expression': ['name', '_bogus_'],
            'fields': ['name']})

    def normWatchmanFileList(self, files):
        # The BSER interface currently returns bytestrings on Python 3 -- decode
        # it into local strings.
        if pywatchman.compat.PYTHON3 and \
            (self.encoding == 'experimental-bser-v2' or \
             self.encoding == 'bser'):
            files = [pywatchman.encoding.decode_local(f) for f in files]
        return sorted(map(norm_path, files))

    def normFileList(self, files):
        return sorted(map(norm_path, files))

    def assertFileListsEqual(self, list1, list2, message=None):
        list1 = [conv_path_to_bytes(f) for f in list1]
        list2 = [conv_path_to_bytes(f) for f in list2]
        self.assertEqual(list1, list2, message)

    def fileListsEqual(self, list1, list2):
        list1 = [conv_path_to_bytes(f) for f in list1]
        list2 = [conv_path_to_bytes(f) for f in list2]
        return list1 == list2

    # Wait for the file list to match the input set
    def assertFileList(self, root, files=[], cursor=None,
                       relativeRoot=None, message=None):
        expected_files = self.normFileList(files)
        if (cursor is not None) and cursor[0:2] == 'n:':
            # it doesn't make sense to repeat named cursor queries, as
            # the cursor moves each time
            self.getFileList(root, cursor=cursor, relativeRoot=relativeRoot)
        else:
            st, res = self.waitFor(
                lambda: self.fileListsEqual(self.getFileList(root, cursor=cursor,
                                            relativeRoot=relativeRoot
                                            ), expected_files))
        self.assertFileListsEqual(self.last_file_list, expected_files, message)

    def waitForSub(self, name, root, accept=None, timeout=10, remove=True):
        client = self.getClient()

        def default_accept(dat):
            return True

        if accept is None:
            accept = default_accept

        deadline = time.time() + timeout
        while time.time() < deadline:
            sub = self.getSubscription(name, root=root, remove=False)
            if sub is not None:
                res = accept(sub)
                if res:
                    return self.getSubscription(name, root=root, remove=remove)
            # wait for more data
            client.setTimeout(deadline - time.time())
            client.receive()

        return None

    def getSubscription(self, name, root, remove=True, normalize=True):
        data = self.getClient().getSubscription(name, root=root, remove=remove)

        if data is None or not normalize:
            return data

        def norm_sub_item(item):
            if isinstance(item, STRING_TYPES):
                return norm_path(item)
            item['name'] = norm_path(item['name'])
            return item

        def norm_sub(sub):
            if 'files' in sub:
                files = []
                for item in sub['files']:
                    files.append(norm_sub_item(item))
                sub['files'] = files
            return sub

        return list(map(norm_sub, data))

    def findSubscriptionContainingFile(self, subdata, filename):
        filename = norm_path(filename)
        for dat in subdata:
            if ('files' in dat and
                filename in self.normWatchmanFileList(dat['files'])):
                return dat
        return None

    def isCaseInsensitive(self):
        if hasattr(self, '_case_insensitive'):
            return self._case_insensitive
        d = self.mkdtemp()
        self.touchRelative(d, 'a')
        self._case_insensitive = os.path.exists(os.path.join(d, 'A'))
        return self._case_insensitive
