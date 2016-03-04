#!/usr/bin/env python
# vim:ts=4:sw=4:et:
import unittest
import os
import os.path
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
import WatchmanTapTests
import WatchmanInstance
import WatchmanTestCase
import NodeTests
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

args = parser.parse_args()

# We test for this in a test case
os.environ['WATCHMAN_EMPTY_ENV_VAR'] = ''

# Ensure that we find the watchman we built in the tests
os.environ['PATH'] = '%s%s%s' % (
        os.path.abspath(os.path.dirname(__file__)),
        os.pathsep,
        os.environ['PATH'])

# We'll put all our temporary stuff under one dir so that we
# can clean it all up at the end
temp_dir = os.path.realpath(tempfile.mkdtemp(prefix='watchmantest'))
# Redirect all temporary files to that location
tempfile.tempdir = temp_dir


def interrupt_handler(signo, frame):
    Interrupt.setInterrupted()
signal.signal(signal.SIGINT, interrupt_handler)

def retry_rmtree(top):
    # Keep trying to remove it; on Windows it may take a few moments
    # for any outstanding locks/handles to be released
    for i in xrange(1, 10):
        shutil.rmtree(top, ignore_errors=True)
        if not os.path.isdir(top):
            return
        time.sleep(0.2)
    sys.stdout.write('Failed to completely remove ' + top)

def cleanup():
    if args.keep:
        sys.stdout.write('Preserving output in %s\n' % temp_dir)
        return
    retry_rmtree(temp_dir)

atexit.register(cleanup)


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

    def setFlavour(self, transport, encoding):
        self.transport = transport
        self.encoding = encoding

    def flavour(self, test):
        if self.transport:
            return '%s [%s, %s]' % (test.id(), self.transport, self.encoding)
        return test.id()

    def addSuccess(self, test):
        elapsed = time.time() - self.startTime
        super(Result, self).addSuccess(test)
        print('\033[32mPASS\033[0m %s (%.3fs)' % (self.flavour(test), elapsed))

    def addSkip(self, test, reason):
        elapsed = time.time() - self.startTime
        super(Result, self).addSkip(test, reason)
        print('\033[33mSKIP\033[0m %s (%.3fs) %s' %
              (self.flavour(test), elapsed, reason))

    def __printFail(self, test, err):
        elapsed = time.time() - self.startTime
        t, val, trace = err
        print('\033[31mFAIL\033[0m %s (%.3fs)\n%s' % (
            self.flavour(test),
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

    def loadTestsFromModule(self, module):
        if not shouldIncludeTestFile(module.__file__):
            return unittest.TestSuite()
        return super(Loader, self).loadTestsFromModule(module)

loader = Loader()
suite = unittest.TestSuite()
for d in ['python/tests', 'tests/integration']:
    suite.addTests(loader.discover(d, top_level_dir=d))

if os.name == 'nt':
    t_globs = 'tests/*.exe'
else:
    t_globs = 'tests/*.t'

suite.addTests(WatchmanTapTests.discover(
    shouldIncludeTestFile, t_globs))
suite.addTests(WatchmanTapTests.discover(
    shouldIncludeTestFile, 'tests/integration/*.php'))

suite.addTests(NodeTests.discover(
    shouldIncludeTestFile, 'node/test/*.js'))
suite.addTests(NodeTests.discover(
    shouldIncludeTestFile, 'tests/integration/*.js'))

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

sys.stdout = ThreadSafeFile(sys.stdout)

tests_queue = queue.Queue()
results_queue = queue.Queue()

def runner():
    global results_queue
    global tests_queue

    broken = False
    try:
        # Start up a shared watchman instance for the tests.
        inst = WatchmanInstance.Instance()
        inst.start()
        # Allow tests to locate this default instance
        WatchmanInstance.setSharedInstance(inst)
    except Exception as e:
        print('while starting watchman: %s' % str(e))
        broken = True

    while True:
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
        elif isinstance(test, WatchmanTestCase.WatchmanTestCase):
            for cfg in test.expandConfigurations():
                target.append(cfg)
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
    shutil.copytree(temp_dir, 'logs')
    subprocess.call(['7z', 'a', 'logs.zip', 'logs'])
    subprocess.call(['appveyor', 'PushArtifact', 'logs.zip'])

if tests_failed or (tests_run == 0):
    if args.keep_if_fail:
        args.keep = True
    sys.exit(1)
