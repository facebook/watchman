# vim:ts=4:sw=4:et:
# Copyright 2012-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0
import errno
import unittest
import pywatchman
import time
import tempfile
import os.path
import os
import WatchmanInstance
import copy


class WatchmanTestCase(unittest.TestCase):

    def getClient(self):
        if not hasattr(self, 'client'):
            self.client = pywatchman.client(
                transport=self.transport,
                sendEncoding=self.encoding,
                recvEncoding=self.encoding,
                sockpath=WatchmanInstance.getSharedInstance().getSockPath())
        return self.client

    def __logTestInfo(self, test, msg):
        if hasattr(self, 'client'):
            try:
                self.getClient().query('log', 'debug',
                                       'TEST: %s %s\n\n' % (test, msg))
            except Exception as e:
                pass

    def run(self, result):
        if result is None:
            raise Exception('MUST be a runtests.py:Result instance')
        # Arrange for any temporary stuff we create to go under
        # our global tempdir and put it in a dir named for the test
        saved_root = tempfile.tempdir
        id = '%s.%s.%s' % (self.id(), self.transport, self.encoding)
        try:
            tempfile.tempdir = os.path.join(saved_root, id)
            os.mkdir(tempfile.tempdir)
            self.__logTestInfo(id, 'BEGIN')
            result.setFlavour(self.transport, self.encoding)
            super(WatchmanTestCase, self).run(result)
        finally:
            tempfile.tempdir = saved_root
            self.__logTestInfo(id, 'END')
            self.__clearWatches()

        return result

    def expandConfigurations(self):
        tests = []
        for transport, encoding in [('local', 'bser'),
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
                self.watchmanCommand('watch-del-all')
            except Exception as e:
                pass

    def __del__(self):
        self.__clearWatches()

    def watchmanCommand(self, *args):
        return self.getClient().query(*args)

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
        files = sorted(res['files'])
        self.last_file_list = files
        return files

    def normFileList(self, files):
        return sorted(map(os.path.normpath, files))

    # Wait for the file list to match the input set
    def assertFileList(self, root, files=[], cursor=None,
                       relativeRoot=None, message=None):
        expected_files = self.normFileList(files)
        st, res = self.waitFor(
            lambda: self.getFileList(root, cursor=cursor,
                                     relativeRoot=relativeRoot
                                     ) == expected_files)
        self.assertEqual(self.last_file_list, expected_files, message)
