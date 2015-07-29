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

import os
import errno
import socket
import subprocess
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


class Transport(object):
    """ communication transport to the watchman server """
    buf = None

    def readBytes(self, size):
        """ read size bytes """
        raise NotImplementedError()

    def write(self, buf):
        """ write some data """
        raise NotImplementedError()

    def readLine(self):
        """ read a line
        Maintains its own buffer, callers of the transport should not mix
        calls to readBytes and readLine.
        """
        if self.buf is None:
            self.buf = []

        while True:
            b = self.readBytes(4096)
            if "\n" in b:
                result = ''.join(self.buf)
                (line, b) = b.split("\n", 1)
                self.buf = [b]
                return result + line
            self.buf.append(b)


class Codec(object):
    """ communication encoding for the watchman server """
    transport = None

    def __init__(self, transport):
        self.transport = transport

    def receive(self):
        raise NotImplementedError()

    def send(self, *args):
        raise NotImplementedError()


class UnixSocketTransport(Transport):
    """ local unix domain socket transport """
    sock = None

    def __init__(self, sockpath, timeout):
        self.sockpath = sockpath
        self.timeout = timeout

        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            sock.settimeout(self.timeout)
            sock.connect(self.sockpath)
            self.sock = sock
        except socket.error as e:
            raise WatchmanError('unable to connect to %s: %s' %
                                (self.sockpath, e))

    def readBytes(self, size):
        try:
            buf = [self.sock.recv(size)]
            if not buf[0]:
                raise WatchmanError('empty watchman response')
            return buf[0]
        except socket.timeout:
            raise WatchmanError('timed out waiting for response')

    def write(self, data):
        try:
            self.sock.sendall(data)
        except socket.timeout:
            raise WatchmanError('timed out sending query command')


class CLIProcessTransport(Transport):
    """ open a pipe to the cli to talk to the service
    This intended to be used only in the test harness!

    The CLI is an oddball because we only support JSON input
    and cannot send multiple commands through the same instance,
    so we spawn a new process for each command.

    We disable server spawning for this implementation, again, because
    it is intended to be used only in our test harness.  You really
    should not need to use the CLI transport for anything real.

    While the CLI can output in BSER, our Transport interface doesn't
    support telling this instance that it should do so.  That effectively
    limits this implementation to JSON input and output only at this time.

    It is the responsibility of the caller to set the send and
    receive codecs appropriately.
    """
    proc = None
    closed = True

    def __init__(self, sockpath, timeout):
        self.sockpath = sockpath
        self.timeout = timeout

    def _connect(self):
        if self.proc:
            return self.proc
        args = [
            'watchman',
            '--sockname={}'.format(self.sockpath),
            '--no-spawn',
            '--no-local',
            '--no-pretty',
            '-j',
        ]
        self.proc = subprocess.Popen(args,
                                     stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE)
        return self.proc

    def readBytes(self, size):
        self._connect()
        res = self.proc.stdout.read(size)
        if res == '':
            raise WatchmanError('EOF on CLI process transport')
        return res

    def write(self, data):
        if self.closed:
            self.closed = False
            self.proc = None
        self._connect()
        res = self.proc.stdin.write(data)
        self.proc.stdin.close()
        self.closed = True
        return res


class BserCodec(Codec):
    """ use the BSER encoding.  This is the default, preferred codec """

    def receive(self):

        buf = [self.transport.readBytes(sniff_len)]
        if not buf[0]:
            raise WatchmanError('empty watchman response')

        elen = bser.pdu_len(buf[0])
        rlen = len(buf[0])

        while elen > rlen:
            buf.append(self.transport.readBytes(elen - rlen))
            rlen += len(buf[-1])

        response = ''.join(buf)
        try:
            return bser.loads(response)
        except ValueError as e:
            raise WatchmanError('watchman response decode error: %s' % e)

    def send(self, *args):
        cmd = bser.dumps(*args)
        self.transport.write(cmd)


class JsonCodec(Codec):
    """ Use json codec.  This is here primarily for testing purposes """
    json = None

    def __init__(self, transport):
        super(JsonCodec, self).__init__(transport)
        # optional dep on json, only if JsonCodec is used
        import json
        self.json = json

    def receive(self):
        line = self.transport.readLine()
        return self.json.loads(line)

    def send(self, *args):
        cmd = self.json.dumps(*args)
        self.transport.write(cmd + "\n")


class client(object):
    """ Handles the communication with the watchman service """
    sockpath = None
    transport = None
    sendCodec = None
    recvCodec = None
    sendConn = None
    recvConn = None

    def __init__(self, sockpath=None, timeout=1.0, transport=None,
                 sendEncoding=None, recvEncoding=None):
        self.sockpath = sockpath
        self.timeout = timeout

        transport = transport or os.getenv('WATCHMAN_TRANSPORT') or 'local'
        if transport == 'local':
            self.transport = UnixSocketTransport
        elif transport == 'cli':
            self.transport = CLIProcessTransport
            if sendEncoding is None:
                sendEncoding = 'json'
            if recvEncoding is None:
                recvEncoding = sendEncoding
        else:
            raise WatchmanError('invalid transport %s' % transport)

        sendEncoding = sendEncoding or os.getenv('WATCHMAN_ENCODING') or 'bser'
        recvEncoding = recvEncoding or os.getenv('WATCHMAN_ENCODING') or 'bser'

        self.recvCodec = self._parseEncoding(recvEncoding)
        self.sendCodec = self._parseEncoding(sendEncoding)

    def _parseEncoding(self, enc):
        if enc == 'bser':
            return BserCodec
        elif enc == 'json':
            return JsonCodec
        else:
            raise WatchmanError('invalid encoding %s' % enc)

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
        except OSError as e:
            raise WatchmanError('"watchman" executable not in PATH (%s)', e)

        stdout, stderr = p.communicate()
        exitcode = p.poll()

        if exitcode:
            raise WatchmanError("watchman exited with code %d" % exitcode)

        result = bser.loads(stdout)
        if 'error' in result:
            raise WatchmanError('get-sockname error: %s' % result['error'])

        return result['sockname']

    def _connect(self):
        """ establish transport connection """

        if self.recvConn:
            return

        if self.sockpath is None:
            self.sockpath = self._resolvesockname()

        transport = self.transport(self.sockpath, self.timeout)
        self.sendConn = self.sendCodec(transport)
        self.recvConn = self.recvCodec(transport)

    def receive(self):
        self._connect()
        result = self.recvConn.receive()
        if 'error' in result:
            raise CommandError(result['error'])

        return result

    def query(self, *args):
        self._connect()
        self.sendConn.send(args)
        return self.receive()
