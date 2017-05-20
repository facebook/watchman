from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import os
import os.path
import subprocess
import inspect
import glob
import re
import WatchmanInstance
import signal
import sys
import Interrupt
import tempfile
import TempDir
import distutils.spawn

try:
    import unittest2 as unittest
except ImportError:
    import unittest

WATCHMAN_SRC_DIR = os.environ.get('WATCHMAN_SRC_DIR', os.getcwd())
THIS_DIR = os.path.join(WATCHMAN_SRC_DIR, 'tests', 'integration')
php_bin = distutils.spawn.find_executable('php')


def find_php_tests(test_class):
    '''
    A decorator function used to create a class per legacy PHP test
    '''

    # We do some rather hacky things here to define new test class types
    # in our caller's scope.  This is needed so that the unittest TestLoader
    # will find the subclasses we define.
    caller_scope = inspect.currentframe().f_back.f_locals
    phprunner = os.path.join(THIS_DIR, 'phprunner')

    for php in glob.glob(os.path.join(THIS_DIR, '*.php')):
        base = os.path.basename(php)
        if base.startswith('.') or base.startswith('_'):
            continue

        subclass_name = base.replace('.', '_').replace('-', '_')

        def make_class(phpfile):
            # Define a new class that derives from the input class.
            # This has to be a function otherwise phpfile captures
            # the value from the last iteration of the glob loop.

            class PHPTest(test_class):
                def getCommandArgs(self):
                    return [
                        php_bin,
                        '-d variables_order=EGPCS',
                        '-d register_argc_argv=1',
                        '-d sys_temp_dir=%s' % TempDir.get_temp_dir().get_dir(),
                        phprunner,
                        phpfile,
                    ]

            # Set the name and module information on our new subclass
            PHPTest.__name__ = subclass_name
            PHPTest.__qualname__ = subclass_name
            PHPTest.__module__ = test_class.__module__

            caller_scope[subclass_name] = PHPTest

        make_class(php)

    return None


@find_php_tests
class PHPTestCase(unittest.TestCase):
    attempt = 0

    def setAttemptNumber(self, attempt):
        ''' enable flaky test retry '''
        self.attempt = attempt

    @unittest.skipIf(php_bin is None, 'php not installed')
    def runTest(self):
        env = os.environ.copy()
        env['WATCHMAN_SOCK'] = WatchmanInstance.getSharedInstance().getSockPath()

        def clean_file_name(name):
            name = name.replace(os.sep, '')
            name = name.replace('tests.integration', '')
            name = name.replace('.php', '')
            name = name.replace('_php', '')
            name = name.replace('.runTest', '')
            return name

        dotted = clean_file_name(os.path.normpath(self.id()))

        if self.attempt > 0:
            dotted += "-%d" % self.attempt

        env['TMPDIR'] = os.path.join(TempDir.get_temp_dir().get_dir(), dotted)
        if os.name != 'nt' and len(env['TMPDIR']) > 94:
            self.fail('temp dir name %s is too long for unix domain sockets' %
                env['TMPDIR'])
        os.mkdir(env['TMPDIR'])
        env['TMP'] = env['TMPDIR']
        env['TEMP'] = env['TMPDIR']
        env['IN_PYTHON_HARNESS'] = '1'
        env['WATCHMAN_PYTHON_BINARY'] = sys.executable
        proc = subprocess.Popen(
            self.getCommandArgs(),
            cwd=env.get('WATCHMAN_SRC_DIR', os.getcwd()),
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE)
        (stdout, stderr) = proc.communicate()
        status = proc.poll()

        if status == -signal.SIGINT:
            Interrupt.setInterrupted()
            self.fail('Interrupted by SIGINT')
            return

        if status != 0:
            self.fail("Exit status %d\n%s\n%s\n" % (status, stdout, stderr))
            return

        res_pat = re.compile(b'^(not )?ok (\d+) (.*)$')
        diag_pat = re.compile(b'^# (.*)$')
        plan_pat = re.compile(b'^1\.\.(\d+)$')

        # Now parse the TAP output
        lines = stdout.replace(b'\r\n', b'\n').split(b'\n')
        last_test = 0
        diags = None
        plan = None

        for line in lines:
            res = plan_pat.match(line)
            if res:
              plan = int(res.group(1))
              continue

            res = res_pat.match(line)
            if res:
                this_test = int(res.group(2))
                if this_test != last_test + 1:
                    print(stdout, stderr)
                    self.fail('Expected test number %d, got %d' % (
                        last_test + 1,
                        this_test))
                last_test = this_test
                if res.group(1) == b'not ':
                    # Failed
                    msg = line
                    if diags is not None:
                        msg = msg + b'\n' + b'\n'.join(diags)
                    self.fail(msg.decode('utf-8'))
                    failed

                diags = None
                continue

            res = diag_pat.match(line)
            if res:
                if diags is None:
                    diags = []
                diags.append(res.group(1))
                continue

            if line != b'':
                print('Invalid tap output from %s: %s' %
                      (self.id(), line))

        if plan is None:
            self.fail('no plan was observed')
        else:
            self.assertEqual(last_test, plan,
                         '%s planned %d but executed %s tests' % (
                             self.id(),
                             plan,
                             last_test))

