import unittest
import os
import os.path
import subprocess
import glob
import re


class TapExeTestCase(unittest.TestCase):

    def __init__(self, executable):
        super(TapExeTestCase, self).__init__()
        self.executable = executable

    def id(self):
        return self.executable

    def run(self, result=None):
        if result is not None:
            result.setFlavour(None, None)
        return super(TapExeTestCase, self).run(result)

    def runTest(self):
        proc = subprocess.Popen(
            [self.executable],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE)
        (stdout, stderr) = proc.communicate()
        status = proc.poll()

        if status != 0:
            self.fail(stdout + '\n' + stderr)
            return

        res_pat = re.compile('^(not )?ok (\d+) (.*)$')
        diag_pat = re.compile('^# (.*)$')

        # Now parse the TAP output
        lines = stdout.split('\n')
        # first line is the plan
        plan = int(lines.pop(0).split('..')[1])
        last_test = 0
        diags = None
        for line in lines:
            res = res_pat.match(line)
            if res:
                this_test = int(res.group(2))
                if this_test != last_test + 1:
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
                      (self.executable, line))

        self.assertEqual(last_test, plan,
                         '%s planned %d but executed %d tests' % (
                             self.executable,
                             plan,
                             last_test))


def discover(path):
    suite = unittest.TestSuite()
    for exe in glob.glob(path):
        suite.addTest(TapExeTestCase(exe))
    return suite
