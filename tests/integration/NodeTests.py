import unittest
import os
import os.path
import subprocess
import glob
import re
import WatchmanInstance
import signal
import Interrupt
import tempfile
import distutils.spawn

node_bin = distutils.spawn.find_executable('node')


class NodeTestCase(unittest.TestCase):

    def __init__(self, jsfile):
        super(NodeTestCase, self).__init__()
        self.jsfile = jsfile

    def id(self):
        return self.jsfile

    def getCommandArgs(self):
        global node_bin
        return [node_bin, self.jsfile]

    @unittest.skipIf(node_bin is None, 'node not installed')
    def runTest(self):
        env = os.environ.copy()
        env['WATCHMAN_SOCK'] = WatchmanInstance.getSharedInstance().getSockPath()
        dotted = os.path.normpath(self.id()).replace(os.sep, '.').replace(
            'tests.integration.', '').replace('.php', '')
        env['TMPDIR'] = os.path.join(tempfile.tempdir, dotted)
        os.mkdir(env['TMPDIR'])
        env['TMP'] = env['TMPDIR']
        env['TEMP'] = env['TMPDIR']
        env['IN_PYTHON_HARNESS'] = '1'
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
            self.fail("Exit status %d\n%s\n%s\n" % (status, stdout, stderr))
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
