# Copyright 2014-present Facebook, Inc.
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

from __future__ import absolute_import
from __future__ import division
from __future__ import print_function
# no unicode literals

import os
import errno
import math
import socket
import subprocess
import time

# Sometimes it's really hard to get Python extensions to compile,
# so fall back to a pure Python implementation.
try:
    from . import bser
except ImportError:
    from . import pybser as bser

from . import (
    capabilities,
)

if os.name == 'nt':
    import ctypes
    import ctypes.wintypes

    wintypes = ctypes.wintypes
    GENERIC_READ = 0x80000000
    GENERIC_WRITE = 0x40000000
    FILE_FLAG_OVERLAPPED = 0x40000000
    OPEN_EXISTING = 3
    INVALID_HANDLE_VALUE = -1
    FORMAT_MESSAGE_FROM_SYSTEM = 0x00001000
    FORMAT_MESSAGE_ALLOCATE_BUFFER = 0x00000100
    FORMAT_MESSAGE_IGNORE_INSERTS = 0x00000200
    WAIT_TIMEOUT = 0x00000102
    WAIT_OBJECT_0 = 0x00000000
    ERROR_IO_PENDING = 997

    class OVERLAPPED(ctypes.Structure):
        _fields_ = [
            ("Internal", wintypes.ULONG), ("InternalHigh", wintypes.ULONG),
            ("Offset", wintypes.DWORD), ("OffsetHigh", wintypes.DWORD),
            ("hEvent", wintypes.HANDLE)
        ]

        def __init__(self):
            self.Offset = 0
            self.OffsetHigh = 0
            self.hEvent = 0

    LPDWORD = ctypes.POINTER(wintypes.DWORD)

    CreateFile = ctypes.windll.kernel32.CreateFileA
    CreateFile.argtypes = [wintypes.LPSTR, wintypes.DWORD, wintypes.DWORD,
                           wintypes.LPVOID, wintypes.DWORD, wintypes.DWORD,
                           wintypes.HANDLE]
    CreateFile.restype = wintypes.HANDLE

    CloseHandle = ctypes.windll.kernel32.CloseHandle
    CloseHandle.argtypes = [wintypes.HANDLE]
    CloseHandle.restype = wintypes.BOOL

    ReadFile = ctypes.windll.kernel32.ReadFile
    ReadFile.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
                         LPDWORD, ctypes.POINTER(OVERLAPPED)]
    ReadFile.restype = wintypes.BOOL

    WriteFile = ctypes.windll.kernel32.WriteFile
    WriteFile.argtypes = [wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
                          LPDWORD, ctypes.POINTER(OVERLAPPED)]
    WriteFile.restype = wintypes.BOOL

    GetLastError = ctypes.windll.kernel32.GetLastError
    GetLastError.argtypes = []
    GetLastError.restype = wintypes.DWORD

    FormatMessage = ctypes.windll.kernel32.FormatMessageA
    FormatMessage.argtypes = [wintypes.DWORD, wintypes.LPVOID, wintypes.DWORD,
                              wintypes.DWORD, ctypes.POINTER(wintypes.LPSTR),
                              wintypes.DWORD, wintypes.LPVOID]
    FormatMessage.restype = wintypes.DWORD

    LocalFree = ctypes.windll.kernel32.LocalFree

    GetOverlappedResultEx = ctypes.windll.kernel32.GetOverlappedResultEx
    GetOverlappedResultEx.argtypes = [wintypes.HANDLE,
                                      ctypes.POINTER(OVERLAPPED), LPDWORD,
                                      wintypes.DWORD, wintypes.BOOL]
    GetOverlappedResultEx.restype = wintypes.BOOL

    CancelIoEx = ctypes.windll.kernel32.CancelIoEx
    CancelIoEx.argtypes = [wintypes.HANDLE, ctypes.POINTER(OVERLAPPED)]
    CancelIoEx.restype = wintypes.BOOL

# 2 bytes marker, 1 byte int size, 8 bytes int64 value
sniff_len = 13

# This is a helper for debugging the client.
_debugging = False
if _debugging:

    def log(fmt, *args):
        print('[%s] %s' %
              (time.strftime("%a, %d %b %Y %H:%M:%S", time.gmtime()),
               fmt % args[:]))
else:

    def log(fmt, *args):
        pass


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

    def __init__(self, msg, cmd=None):
        self.msg = msg
        self.cmd = cmd
        super(CommandError, self).__init__('watchman command error: %s' % msg)

    def setCommand(self, cmd):
        self.cmd = cmd

    def __str__(self):
        if self.cmd:
            return '%s, while executing %s' % (self.msg, self.cmd)
        return self.msg


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

    def setTimeout(self, value):
        pass

    def readLine(self):
        """ read a line
        Maintains its own buffer, callers of the transport should not mix
        calls to readBytes and readLine.
        """
        if self.buf is None:
            self.buf = []

        # Buffer may already have a line if we've received unilateral
        # response(s) from the server
        if len(self.buf) == 1 and b"\n" in self.buf[0]:
            (line, b) = self.buf[0].split(b"\n", 1)
            self.buf = [b]
            return line

        while True:
            b = self.readBytes(4096)
            if b"\n" in b:
                result = b''.join(self.buf)
                (line, b) = b.split(b"\n", 1)
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

    def setTimeout(self, value):
        self.transport.setTimeout(value)


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

    def setTimeout(self, value):
        self.timeout = value
        self.sock.settimeout(self.timeout)

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
        self.timeout = int(math.ceil(timeout * 1000))
        self._iobuf = None

        self.pipe = CreateFile(sockpath, GENERIC_READ | GENERIC_WRITE, 0, None,
                               OPEN_EXISTING, FILE_FLAG_OVERLAPPED, None)

        if self.pipe == INVALID_HANDLE_VALUE:
            self.pipe = None
            self._raise_win_err('failed to open pipe %s' % sockpath,
                                GetLastError())

    def _win32_strerror(self, err):
        """ expand a win32 error code into a human readable message """

        # FormatMessage will allocate memory and assign it here
        buf = ctypes.c_char_p()
        FormatMessage(
            FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_ALLOCATE_BUFFER
            | FORMAT_MESSAGE_IGNORE_INSERTS, None, err, 0, buf, 0, None)
        try:
            return buf.value
        finally:
            LocalFree(buf)

    def _raise_win_err(self, msg, err):
        raise IOError('%s win32 error code: %d %s' %
                      (msg, err, self._win32_strerror(err)))

    def close(self):
        if self.pipe:
            CloseHandle(self.pipe)
        self.pipe = None

    def readBytes(self, size):
        """ A read can block for an unbounded amount of time, even if the
            kernel reports that the pipe handle is signalled, so we need to
            always perform our reads asynchronously
        """

        # try to satisfy the read from any buffered data
        if self._iobuf:
            if size >= len(self._iobuf):
                res = self._iobuf
                self.buf = None
                return res
            res = self._iobuf[:size]
            self._iobuf = self._iobuf[size:]
            return res

        # We need to initiate a read
        buf = ctypes.create_string_buffer(size)
        olap = OVERLAPPED()

        log('made read buff of size %d', size)

        # ReadFile docs warn against sending in the nread parameter for async
        # operations, so we always collect it via GetOverlappedResultEx
        immediate = ReadFile(self.pipe, buf, size, None, olap)

        if not immediate:
            err = GetLastError()
            if err != ERROR_IO_PENDING:
                self._raise_win_err('failed to read %d bytes' % size,
                                    GetLastError())

        nread = wintypes.DWORD()
        if not GetOverlappedResultEx(self.pipe, olap, nread,
                                     0 if immediate else self.timeout, True):
            err = GetLastError()
            CancelIoEx(self.pipe, olap)

            if err == WAIT_TIMEOUT:
                log('GetOverlappedResultEx timedout')
                raise SocketTimeout('timed out after waiting %dms for read' %
                                    self.timeout)

            log('GetOverlappedResultEx reports error %d', err)
            self._raise_win_err('error while waiting for read', err)

        nread = nread.value
        if nread == 0:
            # Docs say that named pipes return 0 byte when the other end did
            # a zero byte write.  Since we don't ever do that, the only
            # other way this shows up is if the client has gotten in a weird
            # state, so let's bail out
            CancelIoEx(self.pipe, olap)
            raise IOError('Async read yielded 0 bytes; unpossible!')

        # Holds precisely the bytes that we read from the prior request
        buf = buf[:nread]

        returned_size = min(nread, size)
        if returned_size == nread:
            return buf

        # keep any left-overs around for a later read to consume
        self._iobuf = buf[returned_size:]
        return buf[:returned_size]

    def write(self, data):
        olap = OVERLAPPED()
        immediate = WriteFile(self.pipe, ctypes.c_char_p(data), len(data),
                              None, olap)

        if not immediate:
            err = GetLastError()
            if err != ERROR_IO_PENDING:
                self._raise_win_err('failed to write %d bytes' % len(data),
                                    GetLastError())

        # Obtain results, waiting if needed
        nwrote = wintypes.DWORD()
        if GetOverlappedResultEx(self.pipe, olap, nwrote, 0 if immediate else
                                 self.timeout, True):
            return nwrote.value

        err = GetLastError()

        # It's potentially unsafe to allow the write to continue after
        # we unwind, so let's make a best effort to avoid that happening
        CancelIoEx(self.pipe, olap)

        if err == WAIT_TIMEOUT:
            raise SocketTimeout('timed out after waiting %dms for write' %
                                self.timeout)
        self._raise_win_err('error while waiting for write of %d bytes' %
                            len(data), err)


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

    def _loads(self, response):
        return bser.loads(response)

    def receive(self):
        buf = [self.transport.readBytes(sniff_len)]
        if not buf[0]:
            raise WatchmanError('empty watchman response')

        elen = bser.pdu_len(buf[0])

        rlen = len(buf[0])
        while elen > rlen:
            buf.append(self.transport.readBytes(elen - rlen))
            rlen += len(buf[-1])

        response = b''.join(buf)
        try:
            res = self._loads(response)
            return res
        except ValueError as e:
            raise WatchmanError('watchman response decode error: %s' % e)

    def send(self, *args):
        cmd = bser.dumps(*args)
        self.transport.write(cmd)


class ImmutableBserCodec(BserCodec):
    """ use the BSER encoding, decoding values using the newer
        immutable object support """

    def _loads(self, response):
        return bser.loads(response, False)


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
    sub_by_root = {}  # Keyed by root, then by subscription name
    logs = []  # When log level is raised
    unilateral = ['log', 'subscription']
    tport = None
    useImmutableBser = None

    def __init__(self,
                 sockpath=None,
                 timeout=1.0,
                 transport=None,
                 sendEncoding=None,
                 recvEncoding=None,
                 useImmutableBser=False):
        self.sockpath = sockpath
        self.timeout = timeout
        self.useImmutableBser = useImmutableBser

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

        sendEncoding = str(sendEncoding or os.getenv('WATCHMAN_ENCODING') or
                           'bser')
        recvEncoding = str(recvEncoding or os.getenv('WATCHMAN_ENCODING') or
                           'bser')

        self.recvCodec = self._parseEncoding(recvEncoding)
        self.sendCodec = self._parseEncoding(sendEncoding)

    def _parseEncoding(self, enc):
        if enc == 'bser':
            if self.useImmutableBser:
                return ImmutableBserCodec
            return BserCodec
        elif enc == 'json':
            return JsonCodec
        else:
            raise WatchmanError('invalid encoding %s' % enc)

    def _hasprop(self, result, name):
        if self.useImmutableBser:
            return hasattr(result, name)
        return name in result

    def _resolvesockname(self):
        # if invoked via a trigger, watchman will set this env var; we
        # should use it unless explicitly set otherwise
        path = os.getenv('WATCHMAN_SOCK')
        if path:
            return path

        cmd = ['watchman', '--output-encoding=bser', 'get-sockname']
        try:
            p = subprocess.Popen(cmd,
                                 stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE,
                                 close_fds=os.name != 'nt')
        except OSError as e:
            raise WatchmanError('"watchman" executable not in PATH (%s)', e)

        stdout, stderr = p.communicate()
        exitcode = p.poll()

        if exitcode:
            raise WatchmanError("watchman exited with code %d" % exitcode)

        result = bser.loads(stdout)
        if b'error' in result:
            raise WatchmanError('get-sockname error: %s' % result['error'])

        return result[b'sockname']

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
        if self._hasprop(result, b'error'):
            raise CommandError(result[b'error'])

        if self._hasprop(result, b'log'):
            self.logs.append(result[b'log'])

        if self._hasprop(result, b'subscription'):
            sub = result[b'subscription']
            if not (sub in self.subs):
                self.subs[sub] = []
            self.subs[sub].append(result)

            # also accumulate in {root,sub} keyed store
            root = os.path.normcase(result[b'root'])
            if not root in self.sub_by_root:
                self.sub_by_root[root] = {}
            if not sub in self.sub_by_root[root]:
                self.sub_by_root[root][sub] = []
            self.sub_by_root[root][sub].append(result)

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

    def getSubscription(self, name, remove=True, root=None):
        """ Retrieve the data associated with a named subscription

        If remove is True (the default), the subscription data is removed
        from the buffer.  Otherwise the data is returned but left in
        the buffer.

        Returns None if there is no data associated with `name`

        If root is not None, then only return the subscription
        data that matches both root and name.  When used in this way,
        remove processing impacts both the unscoped and scoped stores
        for the subscription data.
        """

        if root is not None:
            if not root in self.sub_by_root:
                return None
            if not name in self.sub_by_root[root]:
                return None
            sub = self.sub_by_root[root][name]
            if remove:
                del self.sub_by_root[root][name]
                # don't let this grow unbounded
                if name in self.subs:
                    del self.subs[name]
            return sub

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

        log('calling client.query')
        self._connect()
        try:
            self.sendConn.send(args)

            res = self.receive()
            while self.isUnilateralResponse(res):
                res = self.receive()

            return res
        except CommandError as ex:
            ex.setCommand(args)
            raise ex

    def capabilityCheck(self, optional=None, required=None):
        """ Perform a server capability check """
        res = self.query('version', {
            'optional': optional or [],
            'required': required or []
        })

        if not self._hasprop(res, b'capabilities'):
            # Server doesn't support capabilities, so we need to
            # synthesize the results based on the version
            capabilities.synthesize(res, optional)
            if b'error' in res:
                raise CommandError(res[b'error'])

        return res

    def setTimeout(self, value):
        self.recvConn.setTimeout(value)
        self.sendConn.setTimeout(value)
