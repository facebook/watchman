#!/usr/bin/env python
# vim:ts=4:sw=4:et:
import unittest
import os
import os.path
import sys
import tempfile
import shutil
import subprocess
import traceback
import time
import argparse
import atexit

parser = argparse.ArgumentParser(
    description="Run the watchman unit and integration tests")
parser.add_argument('-v', '--verbosity', default=2,
                    help="test runner verbosity")
parser.add_argument(
    "--keep",
    action='store_true',
    help="preserve all temporary files created during test execution")
args = parser.parse_args()

# Ensure that we can find pywatchman
sys.path.append(os.path.join(os.getcwd(), 'python'))
sys.path.append(os.path.join(os.getcwd(), 'tests/integration'))

# We test for this in a test case
os.environ['WATCHMAN_EMPTY_ENV_VAR'] = ''

unittest.installHandler()

# We'll put all our temporary stuff under one dir so that we
# can clean it all up at the end
temp_dir = tempfile.mkdtemp(prefix='watchmantest')
if args.keep:
    atexit.register(sys.stdout.write,
                    'Preserving output in %s\n' % temp_dir)
else:
    atexit.register(shutil.rmtree, temp_dir)
# Redirect all temporary files to that location
tempfile.tempdir = temp_dir

# Start up a shared watchman instance for the tests.
# We defer the import until after we've modified the python path
import WatchmanInstance
inst = WatchmanInstance.Instance()
inst.start()

# Allow tests to locate our instance by default
os.environ['WATCHMAN_SOCK'] = inst.getSockPath()


class Result(unittest.TestResult):
    # Make it easier to spot success/failure by coloring the status
    # green for pass, red for fail and yellow for skip.
    # also print the elapsed time per test

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


loader = unittest.TestLoader()
suite = unittest.TestSuite()
for d in ['python/tests', 'tests/integration']:
    suite.addTests(loader.discover(d, top_level_dir=d))

unittest.TextTestRunner(
    resultclass=Result,
    verbosity=args.verbosity
).run(suite)
