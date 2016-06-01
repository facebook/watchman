# vim:ts=4:sw=4:et:
# Copyright 2012-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import tempfile
import json
import os.path
import subprocess
import pywatchman
import time
import threading
import uuid
import traceback
import sys

tls = threading.local()

def setSharedInstance(inst):
    global tls
    tls.instance = inst

def getSharedInstance():
    global tls
    return tls.instance

class InitWithFilesMixin(object):
    def _init_state(self):
        self.base_dir = tempfile.mkdtemp(prefix='inst')
        self.cfg_file = os.path.join(self.base_dir, "config.json")
        self.log_file_name = os.path.join(self.base_dir, "log")
        self.cli_log_file_name = os.path.join(self.base_dir, 'cli-log')
        if os.name == 'nt':
            self.sock_file = '\\\\.\\pipe\\watchman-test-%s' % uuid.uuid4().hex
        else:
            self.sock_file = os.path.join(self.base_dir, "sock")
        self.state_file = os.path.join(self.base_dir, "state")

    def get_state_args(self):
        return [
            '--sockname={0}'.format(self.sock_file),
            '--logfile={0}'.format(self.log_file_name),
            '--statefile={0}'.format(self.state_file),
        ]

class _Instance(object):
    # Tracks a running watchman instance.  It is created with an
    # overridden global configuration file; you may pass that
    # in to the constructor

    def __init__(self, config={}, start_timeout=1.0):
        self.start_timeout = start_timeout
        self.base_dir = tempfile.mkdtemp(prefix='inst')
        self._init_state()
        self.proc = None
        self.pid = None
        with open(self.cfg_file, "w") as f:
            f.write(json.dumps(config))
        # The log file doesn't exist right now, so we can't open it.
        self.cli_log_file = open(self.cli_log_file_name, 'w+')

    def __del__(self):
        self.stop()

    def getSockPath(self):
        return self.sock_file

    def stop(self):
        if self.proc:
            self.proc.kill()
            self.proc.wait()
            self.proc = None
        self.cli_log_file.close()

    def start(self):
        args = [
            'watchman',
            '--foreground',
            '--log-level=2',
        ]
        args.extend(self.get_state_args())
        env = os.environ.copy()
        env["WATCHMAN_CONFIG_FILE"] = self.cfg_file
        self.proc = subprocess.Popen(args,
                                     env=env,
                                     stdin=None,
                                     stdout=self.cli_log_file,
                                     stderr=self.cli_log_file)

        # wait for it to come up
        deadline = time.time() + self.start_timeout
        while time.time() < deadline:
            try:
                client = pywatchman.client(sockpath=self.sock_file)
                self.pid = client.query('get-pid')['pid']
                break
            except Exception as e:
                t, val, tb = sys.exc_info()
                time.sleep(0.1)
            finally:
                client.close()

        if self.pid is None:
            pywatchman.compat.reraise(t, val, tb)

class Instance(_Instance, InitWithFilesMixin):
    pass
