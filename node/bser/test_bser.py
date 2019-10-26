#!/usr/bin/env python3
import distutils.spawn
import os
import shutil
import subprocess
import tempfile
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
THIS_DIR = os.path.join(WATCHMAN_SRC_DIR, "tests")


class BserTestCase(unittest.TestCase):
    @unittest.skipIf(
        yarn_bin is None or node_bin is None, "yarn/node not correctly installed"
    )
    def runTest(self):
        with tempfile.TemporaryDirectory() as tempdir:
            env = os.environ.copy()
            env["TMPDIR"] = tempdir

            offline_mirror = env.get("YARN_OFFLINE_MIRROR_PATH_POINTER", None)
            if offline_mirror:
                with open(offline_mirror, "r") as f:
                    mirror = f.read().strip()
                    env["YARN_YARN_OFFLINE_MIRROR"] = mirror
            offline = "YARN_YARN_OFFLINE_MIRROR" in env

            # build the node module with yarn
            node_dir = os.path.join(env["TMPDIR"], "fb-watchman")
            shutil.copytree(os.path.join(WATCHMAN_SRC_DIR, "node"), node_dir)
            bser_dir = os.path.join(node_dir, "bser")

            # install pre-reqs
            install_args = [yarn_bin, "install"]
            if offline:
                install_args.append("--offline")
            print("Installing yarn deps with ", install_args)
            subprocess.check_call(install_args, cwd=bser_dir, env=env)

            env["TMP"] = env["TMPDIR"]
            env["TEMP"] = env["TMPDIR"]
            env["NODE_PATH"] = "%s:%s" % (env["TMPDIR"], env.get("NODE_PATH", ""))
            subprocess.check_call([yarn_bin, "test"], cwd=bser_dir, env=env)
            self.assertTrue(True, "test completed")
