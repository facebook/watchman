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

# Sometimes it's really hard to get Python extensions to compile,
# so fall back to a pure Python implementation.
try:
    import bser
except ImportError, e:
    import pybser as bser

import capabilities

# 2 bytes marker, 1 byte int size, 8 bytes int64 value
sniff_len = 13


class WatchmanError(Exception):
    pass


class SocketTimeout(WatchmanError):
    """A specialized exception raised for socket timeouts during communication to/from watchman.
       This makes it easier to implement non-blocking loops as callers can easily distinguish
       between a routine timeout and an actual error condition.

       Note that catching WatchmanError will also catch this as it is a super-class, so backwards
       compatibility in exception handling is preserved.
    """


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

    def close(self):
        """ tear it down """
        raise NotImplementedError()

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

        # Buffer may already have a line if we've received unilateral
        # response(s) from the server
        if len(self.buf) == 1 and "\n" in self.buf[0]:
            (line, b) = self.buf[0].split("\n", 1)
            self.buf = [b]
            return line

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

    def close(self):
        self.sock.close()
        self.sock = None

    def readBytes(self, size):
        try:
            buf = [self.sock.recv(size)]
            if not buf[0]:
                raise WatchmanError('empty watchman response')
            return buf[0]
        except socket.timeout:
            raise SocketTimeout('timed out waiting for response')

    def write(self, data):
        try:
            self.sock.sendall(data)
        except socket.timeout:
            raise SocketTimeout('timed out sending query command')


class WindowsNamedPipeTransport(Transport):
    """ connect to a named pipe """

    def __init__(self, sockpath, timeout):
        self.sockpath = sockpath
        self.timeout = timeout
        self.pipe = os.open(sockpath, os.O_RDWR | os.O_BINARY)

    def close(self):
        os.close(self.pipe)
        self.pipe = None

    def readBytes(self, size):
        return os.read(self.pipe, size)

    def write(self, data):
        return os.write(self.pipe, data)


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

    def close(self):
        if self.proc:
            self.proc.kill()
            self.proc = None

    def _connect(self):
        if self.proc:
            return self.proc
        args = [
            'watchman',
            '--sockname={}'.format(self.sockpath),
            '--logfile=/BOGUS',
            '--statefile=/BOGUS',
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
            res = bser.loads(response)
            return res
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
        try:
            return self.json.loads(line)
        except Exception as e:
            print(e, line)
            raise

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
    subs = {}  # Keyed by subscription name
    logs = []  # When log level is raised
    unilateral = ['log', 'subscription']
    tport = None

    def __init__(self, sockpath=None, timeout=1.0, transport=None,
                 sendEncoding=None, recvEncoding=None):
        self.sockpath = sockpath
        self.timeout = timeout

        transport = transport or os.getenv('WATCHMAN_TRANSPORT') or 'local'
        if transport == 'local' and os.name == 'nt':
            self.transport = WindowsNamedPipeTransport
        elif transport == 'local':
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
                                 stderr=subprocess.PIPE,
                                 close_fds=os.name != 'nt')
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

        self.tport = self.transport(self.sockpath, self.timeout)
        self.sendConn = self.sendCodec(self.tport)
        self.recvConn = self.recvCodec(self.tport)

    def __del__(self):
        self.close()

    def close(self):
        if self.tport:
            self.tport.close()
            self.tport = None
            self.recvConn = None
            self.sendConn = None

    def receive(self):
        """ receive the next PDU from the watchman service

        If the client has activated subscriptions or logs then
        this PDU may be a unilateral PDU sent by the service to
        inform the client of a log event or subscription change.

        It may also simply be the response portion of a request
        initiated by query.

        There are clients in production that subscribe and call
        this in a loop to retrieve all subscription responses,
        so care should be taken when making changes here.
        """

        self._connect()
        result = self.recvConn.receive()
        if 'error' in result:
            raise CommandError(result['error'])

        if 'log' in result:
            self.logs.append(result['log'])

        if 'subscription' in result:
            sub = result['subscription']
            if not (sub in self.subs):
                self.subs[sub] = []
            self.subs[sub].append(result)

        return result

    def isUnilateralResponse(self, res):
        for k in self.unilateral:
            if k in res:
                return True
        return False

    def getLog(self, remove=True):
        """ Retrieve buffered log data

        If remove is true the data will be removed from the buffer.
        Otherwise it will be left in the buffer
        """
        res = self.logs
        if remove:
            self.logs = []
        return res

    def getSubscription(self, name, remove=True):
        """ Retrieve the data associated with a named subscription

        If remove is True (the default), the subscription data is removed
        from the buffer.  Otherwise the data is returned but left in
        the buffer.

        Returns None if there is no data associated with `name`
        """

        if not (name in self.subs):
            return None
        sub = self.subs[name]
        if remove:
            del self.subs[name]
        return sub

    def query(self, *args):
        """ Send a query to the watchman service and return the response

        This call will block until the response is returned.
        If any unilateral responses are sent by the service in between
        the request-response they will be buffered up in the client object
        and NOT returned via this method.
        """

        self._connect()
        self.sendConn.send(args)

        res = self.receive()
        while self.isUnilateralResponse(res):
            res = self.receive()
        return res

    def capabilityCheck(self, optional=None, required=None):
        """ Perform a server capability check """
        res = self.query('version', {
            'optional': optional or [],
            'required': required or []})

        if not 'capabilities' in res:
            # Server doesn't support capabilities, so we need to
            # synthesize the results based on the version
            capabilities.synthesize(res, opts)
            if 'error' in res:
                raise CommandError(res['error'])

        return res
