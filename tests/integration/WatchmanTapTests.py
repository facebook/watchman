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

class TapExeTestCase(unittest.TestCase):

    def __init__(self, executable):
        super(TapExeTestCase, self).__init__()
        self.executable = executable

    def id(self):
        return self.executable

    def getCommandArgs(self):
        return [self.executable]

    def run(self, result=None):
        if result is not None:
            result.setFlavour(None, None)
        return super(TapExeTestCase, self).run(result)

    def runTest(self):
        env = os.environ.copy()
        env['WATCHMAN_SOCK'] = WatchmanInstance.getSharedInstance().getSockPath()
        dotted = os.path.normpath(self.id()).replace(os.sep, '.').replace(
            'tests.integration.', '')
        env['TMPDIR'] = os.path.join(tempfile.tempdir, dotted)
        if os.name != 'nt' and len(env['TMPDIR']) > 64:
            self.fail('temp dir name is too long for unix domain sockets')
        os.mkdir(env['TMPDIR'])
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

        res_pat = re.compile('^(not )?ok (\d+) (.*)$')
        diag_pat = re.compile('^# (.*)$')

        # Now parse the TAP output
        lines = stdout.split('\n')
        # first line is the plan
        try:
            plan = int(lines.pop(0).split('..')[1])
        except Exception as e:
            self.fail(stdout + '\n' + stderr)
            return
        last_test = 0
        diags = None
        for line in lines:
            res = res_pat.match(line)
            if res:
                this_test = int(res.group(2))
                if this_test != last_test + 1:
                    print(stdout, stderr)
                    self.fail('Expected test number %d, got %d' % (
                        last_test + 1,
                        this_test))
                last_test = this_test
                if res.group(1) == 'not ':
                    # Failed
                    msg = line
                    if diags is not None:
                        msg = msg + '\n'.join(diags)
                    self.fail(msg)
                    failed

                diags = None
                continue

            res = diag_pat.match(line)
            if res:
                if diags is None:
                    diags = []
                diags.append(res.group(1))
                continue

            if line != '':
                print('Invalid tap output from %s: %s' %
                      (self.id(), line))

        self.assertEqual(last_test, plan,
                         '%s planned %d but executed %d tests' % (
                             self.id(),
                             plan,
                             last_test))


class PhutilTestCase(TapExeTestCase):
    def __init__(self, phpfile):
        super(TapExeTestCase, self).__init__()
        self.phpfile = phpfile

    def id(self):
        return self.phpfile

    def getCommandArgs(self):
        if os.name == 'nt':
            arc = 'arc.bat'
        else:
            arc = 'arc'
        return [arc, 'tap', self.phpfile]


def discover(filematcher, path):
    suite = unittest.TestSuite()
    for exe in glob.glob(path):
        if not filematcher(exe):
            continue
        if exe.endswith('.php'):
            suite.addTest(PhutilTestCase(exe))
        else:
            suite.addTest(TapExeTestCase(exe))
    return suite


