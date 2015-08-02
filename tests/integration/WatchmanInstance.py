# vim:ts=4:sw=4:et:
# Copyright 2012-present Facebook, Inc.
# Licensed under the Apache License, Version 2.0
import tempfile
import json
import os.path
import subprocess
import pywatchman
import time
import threading

tls = threading.local()

def setSharedInstance(inst):
    global tls
    tls.instance = inst

def getSharedInstance():
    global tls
    return tls.instance

class Instance(object):
    # Tracks a running watchman instance.  It is created with an
    # overridden global configuration file; you may pass that
    # in to the constructor

    def __init__(self, config={}):
        self.base_dir = tempfile.mkdtemp(prefix='inst')
        self.cfg_file = os.path.join(self.base_dir, "config.json")
        self.log_file_name = os.path.join(self.base_dir, "log")
        self.sock_file = os.path.join(self.base_dir, "sock")
        self.state_file = os.path.join(self.base_dir, "state")
        with open(self.cfg_file, "w") as f:
            f.write(json.dumps(config))
        self.log_file = open(self.log_file_name, 'w+')

    def __del__(self):
        self.stop()

    def getSockPath(self):
        return self.sock_file

    def stop(self):
        if self.proc:
            self.proc.kill()
            self.proc.wait()
            self.proc = None
        self.log_file.close()

    def start(self):
        args = [
            './watchman',
            '--foreground',
            '--sockname={}'.format(self.sock_file),
            '--logfile={}'.format(self.log_file_name),
            '--statefile={}'.format(self.state_file),
            '--log-level=2',
        ]
        env = os.environ.copy()
        env["WATCHMAN_CONFIG_FILE"] = self.cfg_file
        self.proc = subprocess.Popen(args,
                                     env=env,
                                     stdin=None,
                                     stdout=self.log_file,
                                     stderr=self.log_file)

        # wait for it to come up
        last_err = None
        for i in xrange(1, 10):
            try:
                client = pywatchman.client(sockpath=self.sock_file)
                self.pid = client.query('get-pid')['pid']
                break
            except Exception as e:
                last_err = e
                time.sleep(0.1)

        if not self.pid:
            raise last_err
