# Copyright 2012-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import os
import sys
import subprocess


from nbsr import NonBlockingStreamReader
from pywatchman import compat


class Instance(object):
    # Tracks a running WatmanWatch instance

    def __init__(self, sock_path):
        self.sock_file = sock_path
        self.name = None
        self.proc = None

    def __del__(self):
        self.stop()

    def getSockPath(self):
        return self.sock_file

    def stop(self):
        cexc = None

        if self.nbsr:
            try:
                self.nbsr.shutdown()
            except Exception as e:
                cexc = e

        if self.nbesr:
            try:
                self.nbesr.shutdown()
            except Exception as e:
                cexc = e

        if self.proc:
            try:
                self.proc.kill()
            except Exception as e:
                cexc = e

        if self.nbsr:
            try:
                self.nbsr.stop()
            except Exception as e:
                cexc = e

        if self.nbesr:
            try:
                self.nbesr.stop()
            except Exception as e:
                cexc = e

        if self.proc and self.proc.stdout:
            try:
                self.proc.stdout.close()
            except Exception as e:
                cexc = e

        if self.proc and self.proc.stderr:
            try:
                self.proc.stderr.close()
            except Exception as e:
                cexc = e

        self.proc = None

        if cexc:
            raise cexc

    def start(self, paths, maxEvents=1000, cmdArgs=None):
        wait_script = os.environ.get('WATCHMAN_WAIT_PATH')
        if wait_script:
            args = [
                wait_script,
            ]
        else:
            args = [
                sys.executable,
                os.path.join(
                    os.environ['WATCHMAN_PYTHON_BIN'],
                    'watchman-wait'),
            ]
        args.extend(['-m', str(maxEvents)])
        args.extend(paths)

        if cmdArgs:
            args.extend(cmdArgs)
        env = os.environ.copy()
        env["WATCHMAN_SOCK"] = self.getSockPath()
        env["PYTHONPATH"] = env["PYWATCHMAN_PATH"]
        self.proc = subprocess.Popen(args,
                                     env=env,
                                     stdin=None,
                                     stdout=subprocess.PIPE,
                                     stderr=subprocess.PIPE)
        self.nbsr = NonBlockingStreamReader(self.proc.stdout)
        self.nbesr = None

    def getErrors(self, timeout=2):
        # Errors not always retrieved (lazy instantiation)
        if not self.nbesr:
            self.nbesr = NonBlockingStreamReader(self.proc.stderr)
        errMsg = []
        while True:
            msg = self.nbesr.readline(timeout=timeout)
            if not msg:
                break
            errMsg.append(msg)
        return errMsg

    def readLine(self, timeout=10):
        line = self.nbsr.readline(timeout=timeout)
        if not line:
            return line
        if compat.PYTHON3:
            line = line.decode('ascii')
        return line.rstrip()

    def readLines(self, count=1, timeout=10):
        lines = []
        for _ in range(count):
            line = self.readLine(timeout=timeout)
            if not line:
                break
            lines.append(line)
        return lines
