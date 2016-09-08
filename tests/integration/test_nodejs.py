from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

try:
    import unittest2 as unittest
except ImportError:
    import unittest
import os
import os.path
import subprocess
import inspect
import glob
import re
import WatchmanInstance
import TempDir
import signal
import Interrupt
import shutil
import tempfile
import distutils.spawn

node_bin = distutils.spawn.find_executable('node')
npm_bin = distutils.spawn.find_executable('npm')

WATCHMAN_SRC_DIR = os.environ.get('WATCHMAN_SRC_DIR', os.getcwd())
THIS_DIR = os.path.join(WATCHMAN_SRC_DIR, 'tests', 'integration')


def find_js_tests(test_class):
    '''
    A decorator function used to create a class per legacy PHP test
    '''

    # We do some rather hacky things here to define new test class types
    # in our caller's scope.  This is needed so that the unittest TestLoader
    # will find the subclasses we define.
    caller_scope = inspect.currentframe().f_back.f_locals

    for js in glob.glob(os.path.join(THIS_DIR, '*.js')):
        base = os.path.basename(js)
        if base.startswith('.') or base.startswith('_'):
            continue

        subclass_name = base.replace('.', '_').replace('-', '_')

        def make_class(jsfile):
            # Define a new class that derives from the input class.
            # This has to be a function otherwise jsfile captures
            # the value from the last iteration of the glob loop.

            class JSTest(test_class):
                def getCommandArgs(self):
                    return [node_bin, jsfile]

            # Set the name and module information on our new subclass
            JSTest.__name__ = subclass_name
            JSTest.__qualname__ = subclass_name
            JSTest.__module__ = test_class.__module__

            caller_scope[subclass_name] = JSTest

        make_class(js)

    return None


@find_js_tests
class NodeTestCase(unittest.TestCase):

    @unittest.skipIf(node_bin is None or npm_bin is None, 'node not installed')
    def runTest(self):
        env = os.environ.copy()
        env['WATCHMAN_SOCK'] = WatchmanInstance.getSharedInstance().getSockPath()
        dotted = os.path.normpath(self.id()).replace(os.sep, '.').replace(
            'tests.integration.', '').replace('.php', '')
        env['TMPDIR'] = os.path.join(TempDir.get_temp_dir().get_dir(), dotted)
        os.mkdir(env['TMPDIR'])

        # build the node module with npm
        node_dir = os.path.join(env['TMPDIR'], 'fb-watchman')
        shutil.copytree(os.path.join(WATCHMAN_SRC_DIR, 'node'), node_dir)
        subprocess.check_call(['npm', 'install'], cwd=node_dir)

        env['TMP'] = env['TMPDIR']
        env['TEMP'] = env['TMPDIR']
        env['IN_PYTHON_HARNESS'] = '1'
        env['NODE_PATH'] = '%s:%s' % (env['TMPDIR'], env.get('NODE_PATH', ''))
        proc = subprocess.Popen(
            self.getCommandArgs(),
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
            self.fail("Exit status %d\n%s\n%s\n" %
                      (status, stdout.decode('utf-8'), stderr.decode('utf-8')))
            return
        self.assertTrue(True, self.getCommandArgs())


