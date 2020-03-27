# no unicode literals
from __future__ import absolute_import, division, print_function

import distutils.spawn
import glob
import inspect
import os
import os.path
import re
import shutil
import signal
import subprocess

import Interrupt
import WatchmanInstance
import WatchmanTestCase


try:
    import unittest2 as unittest
except ImportError:
    import unittest


def find_node():
    node_bin = os.environ.get("NODE_BIN", distutils.spawn.find_executable("node"))
    if node_bin:
        try:
            subprocess.check_output([node_bin, "-v"])
        except Exception:
            return None
    return node_bin


node_bin = find_node()
yarn_bin = os.environ.get("YARN_PATH", distutils.spawn.find_executable("yarn"))

WATCHMAN_SRC_DIR = os.environ.get("WATCHMAN_SRC_DIR", os.getcwd())
THIS_DIR = os.path.join(WATCHMAN_SRC_DIR, "tests", "integration")


def find_js_tests(test_class):
    """
    A decorator function used to create a class per JavaScript test script
    """

    # We do some rather hacky things here to define new test class types
    # in our caller's scope.  This is needed so that the unittest TestLoader
    # will find the subclasses we define.
    caller_scope = inspect.currentframe().f_back.f_locals

    for js in glob.glob(os.path.join(THIS_DIR, "*.js")):
        base = os.path.basename(js)
        if base.startswith(".") or base.startswith("_"):
            continue

        subclass_name = base.replace(".", "_").replace("-", "_")

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
class NodeTestCase(WatchmanTestCase.TempDirPerTestMixin, unittest.TestCase):
    attempt = 0

    def setAttemptNumber(self, attempt):
        """ enable flaky test retry """
        self.attempt = attempt

    @unittest.skipIf(
        yarn_bin is None or node_bin is None, "yarn/node not correctly installed"
    )
    def runTest(self):
        env = os.environ.copy()
        env["WATCHMAN_SOCK"] = (
            WatchmanInstance.getSharedInstance().getSockPath().legacy_sockpath()
        )
        env["TMPDIR"] = self.tempdir

        offline_mirror = env.get("YARN_OFFLINE_MIRROR_PATH_POINTER", None)
        if offline_mirror:
            with open(offline_mirror, "r") as f:
                mirror = f.read().strip()
                env["YARN_YARN_OFFLINE_MIRROR"] = mirror
        offline = "YARN_YARN_OFFLINE_MIRROR" in env

        # build the node module with yarn
        node_dir = os.path.join(env["TMPDIR"], "fb-watchman")
        shutil.copytree(os.path.join(WATCHMAN_SRC_DIR, "node"), node_dir)

        install_args = [yarn_bin, "install"]
        if offline:
            install_args.append("--offline")

        bser_dir = os.path.join(node_dir, "bser")
        subprocess.check_call(install_args, cwd=bser_dir, env=env)

        env["TMP"] = env["TMPDIR"]
        env["TEMP"] = env["TMPDIR"]
        env["IN_PYTHON_HARNESS"] = "1"
        env["NODE_PATH"] = "%s:%s" % (node_dir, env["TMPDIR"])
        proc = subprocess.Popen(
            self.getCommandArgs(),
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        (stdout, stderr) = proc.communicate()
        status = proc.poll()

        if status == -signal.SIGINT:
            Interrupt.setInterrupted()
            self.fail("Interrupted by SIGINT")
            return

        if status != 0:
            self.fail(
                "Exit status %d\n%s\n%s\n"
                % (status, stdout.decode("utf-8"), stderr.decode("utf-8"))
            )
            return
        self.assertTrue(True, self.getCommandArgs())

    def _getTempDirName(self):
        dotted = (
            os.path.normpath(self.id())
            .replace(os.sep, ".")
            .replace("tests.integration.", "")
            .replace(".php", "")
        )
        if self.attempt > 0:
            dotted += "-%d" % self.attempt
        return dotted
