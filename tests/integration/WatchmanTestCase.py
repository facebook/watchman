# vim:ts=4:sw=4:et:
# Copyright 2012-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import functools
import inspect
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
import TempDir

if pywatchman.compat.PYTHON3:
    STRING_TYPES = (str, bytes)
else:
    STRING_TYPES = (str, unicode)


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

    def __init__(self, methodName='run'):
        super(WatchmanTestCase, self).__init__(methodName)
        self.setDefaultConfiguration()
        self.maxDiff = None
        self.attempt = 0

        if pywatchman.compat.PYTHON3:
            self.assertItemsEqual = self.assertCountEqual

    def requiresPersistentSession(self):
        return False

    def checkPersistentSession(self):
        if self.requiresPersistentSession() and self.transport == 'cli':
            self.skipTest('need persistent session')

    def checkOSApplicability(self):
        # override this to call self.skipTest if this test class should skip
        # on the current OS
        pass

    def setUp(self):
        self.checkPersistentSession()
        self.checkOSApplicability()

    def getClient(self, inst=None):
        if inst or not hasattr(self, 'client'):
            client = pywatchman.client(
                # ASAN-enabled builds can be slower enough that we hit timeouts
                # with the default of 1 second
                timeout=3.0,
                transport=self.transport,
                sendEncoding=self.encoding,
                recvEncoding=self.encoding,
                sockpath=(inst or
                          WatchmanInstance.getSharedInstance()).getSockPath())
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

    def mkdtemp(self, **kwargs):
        return self.normAbsolutePath(tempfile.mkdtemp(dir=self.tempdir,
                                                      **kwargs))

    def mktemp(self, prefix=''):
        f, name = tempfile.mkstemp(prefix=prefix, dir=self.tempdir)
        os.close(f)
        return name

    def setAttemptNumber(self, attempt):
        self.attempt = attempt

    def run(self, result):
        if result is None:
            raise Exception('MUST be a runtests.py:Result instance')

        # Arrange for any temporary stuff we create to go under
        # our global tempdir and put it in a dir named for the test
        id = '%s.%s.%s' % (self.id(), self.transport, self.encoding)
        try:
            self.tempdir = os.path.join(TempDir.get_temp_dir().get_dir(), id)
            if self.attempt > 0:
                self.tempdir += "-%d" % self.attempt
            os.mkdir(self.tempdir)

            self.__logTestInfo(id, 'BEGIN')
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

    def dumpLogs(self):
        ''' used in travis CI to show the hopefully relevant log snippets '''
        inst = WatchmanInstance.getSharedInstance()

        def tail(logstr, n):
            lines = logstr.split('\n')[-n:]
            return '\n'.join(lines)

        print(self.getLogSample())

    def getLogSample(self):
        ''' used in CI to show the hopefully relevant log snippets '''
        inst = WatchmanInstance.getSharedInstance()

        def tail(logstr, n):
            lines = logstr.split('\n')[-n:]
            return '\n'.join(lines)

        return '\n'.join([
            'CLI logs',
            tail(inst.getCLILogContents(), 500),
            'Server logs',
            tail(inst.getServerLogContents(), 500),
        ])

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

    def normRelativePath(self, path):
        # TODO: in the future we will standardize on `/` as the
        # dir separator so we can remove the replace call from here.
        # We do not need to normcase because all of our tests are
        # using the appropriate case already, and watchman returns
        # paths in the canonical file replace case anyway.
        return path.replace('\\', '/')

    def normAbsolutePath(self, path):
        # TODO: in the future we will standardize on `/` as the
        # dir separator so we can remove the replace call.
        return path.replace('\\', '/')

    def _waitForCheck(self, cond, res_check, timeout):
        deadline = time.time() + timeout
        res = None
        while time.time() < deadline:
            res = cond()
            if res_check(res):
                return [True, res]
            time.sleep(0.03)
        return [False, res]

    # Continually invoke `cond` until it returns true or timeout
    # is reached.  Returns a tuple of [bool, result] where the
    # first element of the tuple indicates success/failure and
    # the second element is the return value from the condition
    def waitFor(self, cond, timeout=10):
        return self._waitForCheck(cond, lambda res: res, timeout)

    def waitForEqual(self, expected, actual_cond, timeout=10):
        return self._waitForCheck(
            actual_cond, lambda res: res == expected, timeout
        )

    def assertWaitFor(self, cond, timeout=10, message=None):
        status, res = self.waitFor(cond, timeout)
        if status:
            return res
        if message is None:
            message = "%s was not met in %s seconds: %s" % (cond, timeout, res)
        self.fail(message)

    def assertWaitForEqual(
        self, expected, actual_cond,
        timeout=10, message=None
    ):
        status, res = self.waitForEqual(expected, actual_cond, timeout)
        if status:
            return res
        if message is None:
            message = "%s was not equal to %s in %s seconds: %s" % (
                actual_cond, expected, timeout, res
            )
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

    def getWatchList(self):
        watch_list = self.watchmanCommand('watch-list')['roots']
        self.last_root_list = watch_list
        return watch_list

    def normWatchmanFileList(self, files):
        return sorted(list(map(self.normRelativePath, files)))

    def normFileList(self, files):
        return sorted(list(map(self.normRelativePath, files)))

    def assertFileListsEqual(self, list1, list2, message=None):
        list1 = [self.normRelativePath(f) for f in list1]
        list2 = [self.normRelativePath(f) for f in list2]
        self.assertItemsEqual(list1, list2, message)

    def fileListsEqual(self, list1, list2):
        list1 = [self.normRelativePath(f) for f in list1]
        list2 = [self.normRelativePath(f) for f in list2]
        return sorted(list1) == sorted(list2)

    def fileListContains(self, list1, list2):
        """ return true if list1 contains each unique element in list2 """
        set1 = set([self.normRelativePath(f) for f in list1])
        list2 = [self.normRelativePath(f) for f in list2]
        return set1.issuperset(list2)

    def assertFileListContains(self, list1, list2, message=None):
        if not self.fileListContains(list1, list2):
            self.fail(message)

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

    # Wait for the list of watched roots to match the input set
    def assertWatchListContains(self, roots, message=None):
        st, res = self.waitFor(
            lambda: self.fileListContains(self.getWatchList(), roots))
        self.assertFileListContains(self.last_root_list, roots, message)

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
                return self.normRelativePath(item)
            item['name'] = self.normRelativePath(item['name'])
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
        filename = self.normRelativePath(filename)
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

    def suspendWatchman(self):
        WatchmanInstance.getSharedInstance().suspend()

    def resumeWatchman(self):
        WatchmanInstance.getSharedInstance().resume()

    def rootIsWatched(self, r):
        watches = [self.normAbsolutePath(
            root) for root in self.watchmanCommand('watch-list')['roots']]
        return r in watches


def skip_for(transports=tuple(), codecs=tuple()):
    """
    Decorator to allow skipping tests for particular transports or codecs."""
    transports = set(transports)
    codecs = set(codecs)

    def skip(f):
        @functools.wraps(f)
        def wrapper(self, *args, **kwargs):
            if self.transport in transports or self.encoding in codecs:
                self.skipTest(
                    'test skipped for transport %s, codec %s' %
                    (self.transport, self.encoding)
                )
            return f(self, *args, **kwargs)

        return wrapper
    return skip


def expand_matrix(test_class):
    '''
    A decorator function used to create different permutations from
    a given input test class.

    Given a test class named "MyTest", this will create 4 separate
    classes named "MyTestLocalBser", "MyTestLocalBser2",
    "MyTestLocalJson" and "MyTestCliJson" that will exercise the
    different transport and encoding options implied by their names.
    '''

    matrix = [
        ('local', 'bser', 'LocalBser2'),
        ('local', 'json', 'LocalJson'),
        ('cli', 'json', 'CliJson'),
    ]

    if not pywatchman.compat.PYTHON3:
        matrix += [
            ('local', 'bser-v1', 'LocalBser'),
        ]

    # We do some rather hacky things here to define new test class types
    # in our caller's scope.  This is needed so that the unittest TestLoader
    # will find the subclasses we define.
    caller_scope = inspect.currentframe().f_back.f_locals

    for (transport, encoding, suffix) in matrix:
        def make_class(transport, encoding, suffix):
            subclass_name = test_class.__name__ + suffix

            # Define a new class that derives from the input class
            class MatrixTest(test_class):
                def setDefaultConfiguration(self):
                    self.setConfiguration(transport, encoding)

            # Set the name and module information on our new subclass
            MatrixTest.__name__ = subclass_name
            MatrixTest.__qualname__ = subclass_name
            MatrixTest.__module__ = test_class.__module__

            # Before we publish the test, check whether that generated
            # configuration would always skip
            try:
                t = MatrixTest()
                t.checkPersistentSession()
                t.checkOSApplicability()
                caller_scope[subclass_name] = MatrixTest
            except unittest.SkipTest:
                pass

        make_class(transport, encoding, suffix)

    return None
