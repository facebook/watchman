# Copyright 2014 Facebook, Inc.
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
#  * Redistributions of source code must retain the above copyright notice,
#    this list of conditions and the following disclaimer.
#
#  * Redistributions in binary form must reproduce the above copyright notice,
#    this list of conditions and the following disclaimer in the documentation
#    and/or other materials provided with the distribution.
#
#  * Neither the name Facebook nor the names of its contributors may be used to
#    endorse or promote products derived from this software without specific
#    prior written permission.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
# DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
# FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
# DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
# SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
# CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
# OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

import os, errno, socket, subprocess
import bser

# 2 bytes marker, 1 byte int size, 8 bytes int64 value
sniff_len = 13

class WatchmanError(Exception):
    pass

class CommandError(WatchmanError):
    """error returned by watchman

    self.msg is the message returned by watchman.
    """
    def __init__(self, msg):
        self.msg = msg
        super(CommandError, self).__init__('watchman command error: %s' % msg)

class client(object):
    sock = None
    sockpath = None

    def __init__(self, sockpath = None, timeout = 1.0):
        self.sockpath = sockpath
        self.timeout = timeout

    def _resolvesockname(self):
        # if invoked via a trigger, watchman will set this env var; we
        # should use it unless explicitly set otherwise
        path = os.getenv('WATCHMAN_SOCK')
        if path:
            return path

        cmd = ['watchman', '--output-encoding=bser', 'get-sockname']
        try:
            p = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, close_fds=True)
        except OSError:
            raise WatchmanError('"watchman" executable not in PATH')

        stdout, stderr = p.communicate()
        exitcode = p.poll()

        if exitcode:
            raise WatchmanError("watchman exited with code %d" % exitcode)

        result = bser.loads(stdout)
        if 'error' in result:
            raise WatchmanError('get-sockname error: %s' % result['error'])

        return result['sockname']

    def _connect(self):
        if self.sock:
            return self.sock

        if self.sockpath is None:
            self.sockpath = self._resolvesockname()

        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            sock.settimeout(self.timeout)
            sock.connect(self.sockpath)
            self.sock = sock
            return sock
        except socket.error, e:
            raise WatchmanError('unable to connect to %s: %s' % (self.sockpath, e))

    def receive(self):
        response = None

        try:
            buf = [self.sock.recv(sniff_len)]
            if not buf[0]:
                raise WatchmanError('empty watchman response')

            elen = bser.pdu_len(buf[0])
            rlen = len(buf[0])

            while elen > rlen:
                buf.append(self.sock.recv(elen - rlen))
                rlen += len(buf[-1])

            response = ''.join(buf)
        except socket.timeout:
            raise WatchmanError('timed out waiting for response')

        result = None

        try:
            result = bser.loads(response)
        except ValueError, e:
            raise WatchmanError('watchman response decode error: %s' % e)

        if 'error' in result:
            raise CommandError(result['error'])

        return result

    def query(self, *args):
        cmd = bser.dumps(args)
        sock = self._connect()
        try:
            sock.sendall(cmd)
        except socket.timeout:
            raise WatchmanError('timed out sending query command')
        return self.receive()
