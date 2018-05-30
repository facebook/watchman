#!/usr/bin/env python3
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
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS 'AS IS'
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
# DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
# FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
# DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
# SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
# CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
# OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

from __future__ import absolute_import, division, print_function

# no unicode literals
import asyncio
import os
import subprocess

from pywatchman import CommandError, WatchmanError, encoding


try:
    from pywatchman import bser
except ImportError:
    from pywatchman import pybser as bser


# 2 bytes marker, 1 byte int size, 8 bytes int64 value
SNIFF_LEN = 13


# TODO: Fix this when https://github.com/python/asyncio/issues/281 is resolved.
# tl;dr is that you cannot have different event loops running in different
# threads all fork subprocesses and listen for child events. The current
# workaround is to do the old fashioned blocking process communication using a
# ThreadPool.
def _resolve_sockname_helper():
    # if invoked via a trigger, watchman will set this env var; we
    # should use it unless explicitly set otherwise
    path = os.getenv("WATCHMAN_SOCK")
    if path:
        return path

    cmd = ["watchman", "--output-encoding=bser", "get-sockname"]

    try:
        p = subprocess.Popen(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            close_fds=os.name != "nt",
        )
    except OSError as e:
        raise WatchmanError('"watchman" executable not in PATH (%s)', e)

    stdout, stderr = p.communicate()
    exitcode = p.poll()

    if exitcode:
        raise WatchmanError("watchman exited with code %d" % exitcode)

    result = bser.loads(stdout)

    if "error" in result:
        raise WatchmanError(str(result["error"]))

    return result["sockname"]


async def _resolve_sockname():
    """Find the Unix socket path to the global Watchman instance."""
    loop = asyncio.get_event_loop()
    return await loop.run_in_executor(None, _resolve_sockname_helper)


class AsyncTransport(object):
    """Communication transport to the Watchman Service."""

    async def activate(self, **kwargs):
        """Make the transport ready for use. Optional for subclasses."""
        pass

    async def read(self, size):
        """Read 'size' bytes from the transport."""
        raise NotImplementedError()

    async def write(self, buf):
        """Write 'buf' bytes to the transport."""
        raise NotImplementedError()

    def close(self):
        """Close the transport. Optional for subclasses."""
        pass


class AsyncUnixSocketTransport(AsyncTransport):
    """Local Unix domain socket transport supporting asyncio."""

    def __init__(self):
        self.sockname = None
        self.reader = None
        self.writer = None

    async def activate(self, **kwargs):
        # Requires keyword-argument 'sockname'
        reader, writer = await asyncio.open_unix_connection(kwargs["sockname"])
        self.reader = reader
        self.writer = writer

    async def write(self, data):
        self.writer.write(data)
        await self.writer.drain()

    async def read(self, size):
        res = await self.reader.read(size)
        return res

    def close(self):
        if self.writer:
            self.writer.close()


class AsyncCodec(object):
    """Communication encoding for the Watchman service."""

    def __init__(self, transport):
        self.transport = transport

    async def receive(self):
        """Read from the underlying transport, parse and return the message."""
        raise NotImplementedError()

    async def send(self, *args):
        """Send the given message via the underlying transport."""
        raise NotImplementedError()

    def close(self):
        """Close the underlying transport."""
        self.transport.close()


# This requires BSERv2 support of the server, but doesn't gracefully check
# for the requisite capability being present in older versions.
class AsyncBserCodec(AsyncCodec):
    """Use the BSER encoding."""

    async def receive(self):
        sniff = await self.transport.read(SNIFF_LEN)
        if not sniff:
            raise WatchmanError("empty watchman response")
        _1, _2, elen = bser.pdu_info(sniff)
        rlen = len(sniff)
        buf = bytearray(elen)
        buf[:rlen] = sniff
        while elen > rlen:
            b = await self.transport.read(elen - rlen)
            buf[rlen : rlen + len(b)] = b
            rlen += len(b)
        response = bytes(buf)
        try:
            res = self._loads(response)
            return res
        except ValueError as e:
            raise WatchmanError("watchman response decode error: %s" % e)

    async def send(self, *args):
        cmd = bser.dumps(*args, version=2, capabilities=0)
        await self.transport.write(cmd)

    def _loads(self, response):
        """ Parse the BSER packet """
        return bser.loads(
            response,
            True,
            value_encoding=encoding.get_local_encoding(),
            value_errors=encoding.default_local_errors,
        )


class ReceiveLoopError(Exception):
    pass


class AIOClient(object):
    def __init__(self, connection):
        self.connection = connection
        self.log_queue = asyncio.Queue()
        self.sub_by_root = {}
        self.bilateral_response_queue = asyncio.Queue()
        self.recieive_task = None
        self.receive_task_exception = None

    def stop(self):
        self.should_stop = True

    async def receive_bilateral_response(self):
        """Receive the response to a request made to the Watchman service."""

        self._check_receive_loop()
        resp = await self.bilateral_response_queue.get()
        self._check_error(resp)
        return resp

    async def query(self, *args):
        """Send a query to the Watchman service and return the response."""

        self._check_receive_loop()
        try:
            await self.connection.send(args)
            return await self.receive_bilateral_response()
        except CommandError as ex:
            ex.setCommand(args)
            raise ex

    async def capability_check(self, optional=None, required=None):
        """Perform a server capability check."""

        self._check_receive_loop()
        # If the returned response is an error, self.query will raise an error
        await self.query(
            "version", {"optional": optional or [], "required": required or []}
        )

    async def get_subscription(self, name, root):
        """ Retrieve the data associated with a named subscription

        Returns None if there is no data associated with `name`

        If root is not None, then only return the subscription
        data that matches both root and name.  When used in this way,
        remove processing impacts both the unscoped and scoped stores
        for the subscription data.
        """
        self._check_receive_loop()
        self._ensure_subscription_queue_exists(name, root)
        return await self.sub_by_root[root][name].get()

    async def pop_log(self):
        """Get one log from the log queue."""
        self._check_receive_loop()
        return await self.log_queue.get()

    def close(self):
        """Close the underlying connection."""
        if self.receive_task:
            self.receive_task.cancel()
        if self.connection:
            self.connection.close()

    def enable_receiving(self, loop=None):
        """Schedules the receive loop to run on the given loop."""
        self.receive_task = asyncio.ensure_future(self._receive_loop(), loop=loop)

        def do_if_done(fut):
            try:
                fut.result()
            except asyncio.CancelledError:
                pass
            except Exception as ex:
                self.receive_task_exception = ex

        self.receive_task.add_done_callback(do_if_done)

    @classmethod
    async def from_socket(cls, sockname=None):
        """Create a new AIOClient using Unix transport and BSER Codec
        connecting to the specified socket. If the specified socket is None,
        then resolve the socket path automatically.

        This method also schedules the receive loop to run on the event loop.

        This method is a coroutine."""
        if not sockname:
            sockname = await _resolve_sockname()
        transport = AsyncUnixSocketTransport()
        await transport.activate(sockname=sockname)
        connection = AsyncBserCodec(transport)
        obj = cls(connection)
        obj.enable_receiving()
        return obj

    async def _receive_loop(self):
        """Receive the response to a request made to the Watchman service.

        Note that when trying to receive a PDU from the Watchman service,
        we might get a unilateral response to a subscription or log, so these
        are processed and queued up for later retrieval. This function only
        returns when a non-unilateral response is received."""

        while True:
            response = await self.connection.receive()
            if self._is_unilateral(response):
                await self._process_unilateral_response(response)
            else:
                await self.bilateral_response_queue.put(response)

    def _check_error(self, res):
        if "error" in res:
            raise CommandError(res["error"])

    def _check_receive_loop(self):
        if self.receive_task is None:
            raise ReceiveLoopError("Receive loop was not started.")

    def _is_unilateral(self, res):
        return res.get("unilateral") or "subscription" in res or "log" in res

    def _ensure_subscription_queue_exists(self, name, root):
        # Note this function must be called from an async function on only one
        # event loop.
        self.sub_by_root.setdefault(root, {}).setdefault(name, asyncio.Queue())

    async def _process_unilateral_response(self, response):
        if "log" in response:
            await self.logs.put(response["log"])

        elif "subscription" in response:
            sub = response["subscription"]
            root = os.path.normcase(response["root"])
            self._ensure_subscription_queue_exists(sub, root)
            await self.sub_by_root[root][sub].put(response)

        elif self._is_unilateral(response):
            raise WatchmanError("Unknown unilateral response: " + str(response))

        else:
            raise WatchmanError("Not a unilateral response: " + str(response))
