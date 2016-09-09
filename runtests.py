#!/usr/bin/env python
# vim:ts=4:sw=4:et:

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals
import os
import os.path

# in the FB internal test infra, ensure that we are running from the
# dir that houses this script rather than some other higher level dir
# in the containing tree.  We can't use __file__ to determine this
# because our PAR machinery can generate a name like /proc/self/fd/3/foo
# which won't resolve to anything useful by the time we get here.
if not os.path.exists('runtests.py') and os.path.exists('watchman/runtests.py'):
    os.chdir('watchman')

try:
    import unittest2 as unittest
except ImportError:
    import unittest
import sys
# Ensure that we can find pywatchman
sys.path.insert(0, os.path.join(os.getcwd(), 'python'))
sys.path.insert(0, os.path.join(os.getcwd(), 'tests', 'integration'))
import tempfile
import shutil
import subprocess
import traceback
import time
import argparse
import atexit
import WatchmanInstance
import WatchmanTestCase
import TempDir
import glob
import threading
import multiprocessing
import math
import signal
import Interrupt
import random

try:
    import queue
except Exception:
    import Queue
    queue = Queue

parser = argparse.ArgumentParser(
    description="Run the watchman unit and integration tests")
parser.add_argument('-v', '--verbosity', default=2,
                    help="test runner verbosity")
parser.add_argument(
    "--keep",
    action='store_true',
    help="preserve all temporary files created during test execution")
parser.add_argument(
    "--keep-if-fail",
    action='store_true',
    help="preserve all temporary files created during test execution if failed")

parser.add_argument(
    "files",
    nargs='*',
    help='specify which test files to run')

parser.add_argument(
    '--method',
    action='append',
    help='specify which python test method names to run')

parser.add_argument(
    '--concurrency',
    default=int(min(8, math.ceil(1.5 * multiprocessing.cpu_count()))),
    type=int,
    help='How many tests to run at once')

parser.add_argument(
    '--watcher',
    action='store',
    default='auto',
    help='Specify which watcher should be used to run the tests')

parser.add_argument(
    '--debug-watchman',
    action='store_true',
    help='Pauses start up and prints out the PID for watchman server process.' +
    'Use with concurrency set to 1')

parser.add_argument(
    '--watchman-path',
    action='store',
    help='Specify the path to the watchman binary')

args = parser.parse_args()

# We test for this in a test case
os.environ['WATCHMAN_EMPTY_ENV_VAR'] = ''

# Ensure that we find the watchman we built in the tests
if args.watchman_path:
    bin_dir = os.path.dirname(args.watchman_path)
else:
    bin_dir = os.path.dirname(__file__)

os.environ['PATH'] = '%s%s%s' % (os.path.abspath(bin_dir),
                                 os.pathsep,
                                 os.environ['PATH'])

# We'll put all our temporary stuff under one dir so that we
# can clean it all up at the end
temp_dir = TempDir.get_temp_dir(args.keep)

def interrupt_handler(signo, frame):
    Interrupt.setInterrupted()
signal.signal(signal.SIGINT, interrupt_handler)


class Result(unittest.TestResult):
    # Make it easier to spot success/failure by coloring the status
    # green for pass, red for fail and yellow for skip.
    # also print the elapsed time per test
    transport = None
    encoding = None

    def shouldStop(self):
        if Interrupt.wasInterrupted():
            return True
        return super(Result, self).shouldStop()

    def startTest(self, test):
        self.startTime = time.time()
        super(Result, self).startTest(test)

    def addSuccess(self, test):
        elapsed = time.time() - self.startTime
        super(Result, self).addSuccess(test)
        print('\033[32mPASS\033[0m %s (%.3fs)' % (test.id(), elapsed))

    def addSkip(self, test, reason):
        elapsed = time.time() - self.startTime
        super(Result, self).addSkip(test, reason)
        print('\033[33mSKIP\033[0m %s (%.3fs) %s' %
              (test.id(), elapsed, reason))

    def __printFail(self, test, err):
        elapsed = time.time() - self.startTime
        t, val, trace = err
        print('\033[31mFAIL\033[0m %s (%.3fs)\n%s' % (
            test.id(),
            elapsed,
            ''.join(traceback.format_exception(t, val, trace))))

    def addFailure(self, test, err):
        self.__printFail(test, err)
        super(Result, self).addFailure(test, err)

    def addError(self, test, err):
        self.__printFail(test, err)
        super(Result, self).addError(test, err)


def expandFilesList(files):
    """ expand any dir names into a full list of files """
    res = []
    for g in args.files:
        if os.path.isdir(g):
            for dirname, dirs, files in os.walk(g):
                for f in files:
                    if not f.startswith('.'):
                        res.append(os.path.normpath(os.path.join(dirname, f)))
        else:
            res.append(os.path.normpath(g))
    return res

if args.files:
    args.files = expandFilesList(args.files)


def shouldIncludeTestFile(filename):
    """ used by our loader to respect the set of tests to run """
    global args
    fname = os.path.relpath(filename.replace('.pyc', '.py'))
    if args.files:
        for f in args.files:
            if f == fname:
                return True
        return False

    if args.method:
        # implies python tests only
        if not fname.endswith('.py'):
            return False

    return True

def shouldIncludeTestName(name):
    """ used by our loader to respect the set of tests to run """
    global args
    if args.method:
        method = name.split('.').pop()
        for f in args.method:
            if method == f:
                return True
        return False
    return True


class Loader(unittest.TestLoader):
    """ allows us to control the subset of which tests are run """

    def __init__(self):
        super(Loader, self).__init__()

    def loadTestsFromTestCase(self, testCaseClass):
        return super(Loader, self).loadTestsFromTestCase(testCaseClass)

    def getTestCaseNames(self, testCaseClass):
        names = super(Loader, self).getTestCaseNames(testCaseClass)
        return filter(lambda name: shouldIncludeTestName(name), names)

    def loadTestsFromModule(self, module, *args, **kw):
        if not shouldIncludeTestFile(module.__file__):
            return unittest.TestSuite()
        return super(Loader, self).loadTestsFromModule(module, *args, **kw)

loader = Loader()
suite = unittest.TestSuite()
for d in ['python/tests', 'tests/integration']:
    suite.addTests(loader.discover(d, top_level_dir=d))

if os.name == 'nt':
    t_globs = 'tests/*.exe'
else:
    t_globs = 'tests/*.t'

# Manage printing from concurrent threads
# http://stackoverflow.com/a/3030755/149111
class ThreadSafeFile(object):
    def __init__(self, f):
        self.f = f
        self.lock = threading.RLock()
        self.nesting = 0

    def _getlock(self):
        self.lock.acquire()
        self.nesting += 1

    def _droplock(self):
        nesting = self.nesting
        self.nesting = 0
        for i in range(nesting):
            self.lock.release()

    def __getattr__(self, name):
        if name == 'softspace':
            return tls.softspace
        else:
            raise AttributeError(name)

    def __setattr__(self, name, value):
        if name == 'softspace':
            tls.softspace = value
        else:
            return object.__setattr__(self, name, value)

    def write(self, data):
        self._getlock()
        self.f.write(data)
        if data == '\n':
            self._droplock()

    def flush(self):
        self._getlock()
        self.f.flush()
        self._droplock()

sys.stdout = ThreadSafeFile(sys.stdout)

tests_queue = queue.Queue()
results_queue = queue.Queue()

def runner():
    global results_queue
    global tests_queue

    broken = False
    try:
        # Start up a shared watchman instance for the tests.
        inst = WatchmanInstance.Instance({
            "watcher": args.watcher
        }, debug_watchman=args.debug_watchman)
        inst.start()
        # Allow tests to locate this default instance
        WatchmanInstance.setSharedInstance(inst)
    except Exception as e:
        print('while starting watchman: %s' % str(e))
        traceback.print_exc()
        broken = True

    while not broken:
        test = tests_queue.get()
        try:
            if test == 'terminate':
                break

            if Interrupt.wasInterrupted() or broken:
                continue

            try:
                result = Result()
                test.run(result)
                results_queue.put(result)
            except Exception as e:
                print(e)

        finally:
            tests_queue.task_done()

    if not broken:
        inst.stop()

def expand_suite(suite, target=None):
    """ recursively expand a TestSuite into a list of TestCase """
    if target is None:
        target = []
    for i, test in enumerate(suite):
        if isinstance(test, unittest.TestSuite):
            expand_suite(test, target)
        else:
            target.append(test)

    # randomize both because we don't want tests to have relatively
    # dependency ordering and also because this can help avoid clumping
    # longer running tests together
    random.shuffle(target)
    return target

def queue_jobs(tests):
    for test in tests:
        tests_queue.put(test)

all_tests = expand_suite(suite)
if len(all_tests) < args.concurrency:
    args.concurrency = len(all_tests)
queue_jobs(all_tests)

for i in range(args.concurrency):
    t = threading.Thread(target=runner)
    t.daemon = True
    t.start()
    # also send a termination sentinel
    tests_queue.put('terminate')

# Wait for all tests to have been dispatched
tests_queue.join()

# Now pull out and aggregate the results
tests_run = 0
tests_failed = 0
tests_skipped = 0
while not results_queue.empty():
    res = results_queue.get()
    tests_run = tests_run + res.testsRun
    tests_failed = tests_failed + len(res.errors) + len(res.failures)
    tests_skipped = tests_skipped + len(res.skipped)

print('Ran %d, failed %d, skipped %d, concurrency %d' % (
    tests_run, tests_failed, tests_skipped, args.concurrency))

if 'APPVEYOR' in os.environ:
    shutil.copytree(temp_dir.get_dir(), 'logs')
    subprocess.call(['7z', 'a', 'logs.zip', 'logs'])
    subprocess.call(['appveyor', 'PushArtifact', 'logs.zip'])

if tests_failed or (tests_run == 0):
    if args.keep_if_fail:
        temp_dir.set_keep(True)
    sys.exit(1)
