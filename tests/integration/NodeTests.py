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
import glob
import re
import WatchmanInstance
import signal
import Interrupt
import shutil
import tempfile
import distutils.spawn

node_bin = distutils.spawn.find_executable('node')
npm_bin = distutils.spawn.find_executable('npm')

class NodeTestCase(unittest.TestCase):

    def __init__(self, jsfile):
        super(NodeTestCase, self).__init__()
        self.jsfile = jsfile

    def id(self):
        return self.jsfile

    def getCommandArgs(self):
        global node_bin
        return [node_bin, self.jsfile]

    @unittest.skipIf(node_bin is None or npm_bin is None, 'node not installed')
    def runTest(self):
        env = os.environ.copy()
        env['WATCHMAN_SOCK'] = WatchmanInstance.getSharedInstance().getSockPath()
        dotted = os.path.normpath(self.id()).replace(os.sep, '.').replace(
            'tests.integration.', '').replace('.php', '')
        env['TMPDIR'] = os.path.join(tempfile.tempdir, dotted)
        os.mkdir(env['TMPDIR'])

        # build the node module with npm
        node_dir = os.path.join(env['TMPDIR'], 'fb-watchman')
        shutil.copytree('node', node_dir)
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
        self.assertTrue(True, self.jsfile)


def discover(filematcher, path):
    suite = unittest.TestSuite()

    for jsfile in glob.glob(path):
        if not filematcher(jsfile):
            continue
        base = os.path.basename(jsfile)
        if base.startswith('.') or base.startswith('_'):
            continue
        suite.addTest(NodeTestCase(jsfile))
    return suite
