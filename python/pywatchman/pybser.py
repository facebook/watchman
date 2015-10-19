# Copyright 2015 Facebook, Inc.
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

import collections
import ctypes
import struct

BSER_ARRAY = '\x00'
BSER_OBJECT = '\x01'
BSER_STRING = '\x02'
BSER_INT8 = '\x03'
BSER_INT16 = '\x04'
BSER_INT32 = '\x05'
BSER_INT64 = '\x06'
BSER_REAL = '\x07'
BSER_TRUE = '\x08'
BSER_FALSE = '\x09'
BSER_NULL = '\x0a'
BSER_TEMPLATE = '\x0b'
BSER_SKIP = '\x0c'

# Leave room for the serialization header, which includes
# our overall length.  To make things simpler, we'll use an
# int32 for the header
EMPTY_HEADER = "\x00\x01\x05\x00\x00\x00\x00"


def _int_size(x):
    """Return the smallest size int that can store the value"""
    abs_x = abs(x)
    if abs_x <= 0x7F:
        return 1
    elif abs_x <= 0x7FFF:
        return 2
    elif abs_x <= 0x7FFFFFFF:
        return 4
    elif abs_x <= 0x7FFFFFFFFFFFFFFFL:
        return 8
    else:
        raise RuntimeException('Cannot represent value: ' + str(x))


class _bser_buffer(object):

    def __init__(self):
        self.buf = ctypes.create_string_buffer(8192)
        struct.pack_into(str(len(EMPTY_HEADER)) + 's', self.buf, 0, EMPTY_HEADER)
        self.wpos = len(EMPTY_HEADER)

    def ensure_size(self, size):
        while ctypes.sizeof(self.buf) - self.wpos < size:
            ctypes.resize(self.buf, ctypes.sizeof(self.buf) * 2)

    def append_long(self, val):
        size = _int_size(val)
        to_write = size + 1
        self.ensure_size(to_write)
        if size == 1:
            struct.pack_into('=cb', self.buf, self.wpos, BSER_INT8, val)
        elif size == 2:
            struct.pack_into('=ch', self.buf, self.wpos, BSER_INT16, val)
        elif size == 4:
            struct.pack_into('=ci', self.buf, self.wpos, BSER_INT32, val)
        elif size == 8:
            struct.pack_into('=cq', self.buf, self.wpos, BSER_INT64, val)
        else:
            raise RuntimeError('Cannot represent this long value')
        self.wpos += to_write


    def append_string(self, s):
        if isinstance(s, unicode):
            s = s.encode('utf-8')
        s_len = len(s)
        size = _int_size(s_len)
        to_write = 2 + size + s_len
        self.ensure_size(to_write)
        if size == 1:
            struct.pack_into('=ccb' + str(s_len) + 's', self.buf, self.wpos, BSER_STRING, BSER_INT8, s_len, s)
        elif size == 2:
            struct.pack_into('=cch' + str(s_len) + 's', self.buf, self.wpos, BSER_STRING, BSER_INT16, s_len, s)
        elif size == 4:
            struct.pack_into('=cci' + str(s_len) + 's', self.buf, self.wpos, BSER_STRING, BSER_INT32, s_len, s)
        elif size == 8:
            struct.pack_into('=ccq' + str(s_len) + 's', self.buf, self.wpos, BSER_STRING, BSER_INT64, s_len, s)
        else:
            raise RuntimeError('Cannot represent this string value')
        self.wpos += to_write


    def append_recursive(self, val):
        if isinstance(val, bool):
            needed = 1
            self.ensure_size(needed)
            if val:
                to_encode = BSER_TRUE
            else:
                to_encode = BSER_FALSE
            struct.pack_into('=c', self.buf, self.wpos, to_encode)
            self.wpos += needed
        elif val is None:
            needed = 1
            self.ensure_size(needed)
            struct.pack_into('=c', self.buf, self.wpos, BSER_NULL)
            self.wpos += needed
        elif isinstance(val, (int, long)):
            self.append_long(val)
        elif isinstance(val, (str, unicode)):
            self.append_string(val)
        elif isinstance(val, float):
            needed = 9
            self.ensure_size(needed)
            struct.pack_into('=cd', self.buf, self.wpos, BSER_REAL, val)
            self.wpos += needed
        elif isinstance(val, collections.Mapping) and isinstance(val, collections.Sized):
            val_len = len(val)
            size = _int_size(val_len)
            needed = 2 + size
            self.ensure_size(needed)
            if size == 1:
                struct.pack_into('=ccb', self.buf, self.wpos, BSER_OBJECT, BSER_INT8, val_len)
            elif size == 2:
                struct.pack_into('=cch', self.buf, self.wpos, BSER_OBJECT, BSER_INT16, val_len)
            elif size == 4:
                struct.pack_into('=cci', self.buf, self.wpos, BSER_OBJECT, BSER_INT32, val_len)
            elif size == 8:
                struct.pack_into('=ccq', self.buf, self.wpos, BSER_OBJECT, BSER_INT64, val_len)
            else:
                raise RuntimeError('Cannot represent this mapping value')
            self.wpos += needed
            for k, v in val.iteritems():
                self.append_string(k)
                self.append_recursive(v)
        elif isinstance(val, collections.Iterable) and isinstance(val, collections.Sized):
            val_len = len(val)
            size = _int_size(val_len)
            needed = 2 + size
            self.ensure_size(needed)
            if size == 1:
                struct.pack_into('=ccb', self.buf, self.wpos, BSER_ARRAY, BSER_INT8, val_len)
            elif size == 2:
                struct.pack_into('=cch', self.buf, self.wpos, BSER_ARRAY, BSER_INT16, val_len)
            elif size == 4:
                struct.pack_into('=cci', self.buf, self.wpos, BSER_ARRAY, BSER_INT32, val_len)
            elif size == 8:
                struct.pack_into('=ccq', self.buf, self.wpos, BSER_ARRAY, BSER_INT64, val_len)
            else:
                raise RuntimeError('Cannot represent this sequence value')
            self.wpos += needed
            for v in val:
                self.append_recursive(v)
        else:
            raise RuntimeError('Cannot represent unknown value type')


def dumps(obj):
    bser_buf = _bser_buffer()
    bser_buf.append_recursive(obj)
    # Now fill in the overall length
    obj_len = bser_buf.wpos - len(EMPTY_HEADER)
    struct.pack_into('=i', bser_buf.buf, 3, obj_len)
    return bser_buf.buf.raw[:bser_buf.wpos]


def _bunser_int(buf, pos):
    int_type = buf[pos]
    if int_type == BSER_INT8:
        needed = 2
        fmt = '=b'
    elif int_type == BSER_INT16:
        needed = 3
        fmt = '=h'
    elif int_type == BSER_INT32:
        needed = 5
        fmt = '=i'
    elif int_type == BSER_INT64:
        needed = 9
        fmt = '=q'
    else:
        raise RuntimeError('Invalid bser int encoding 0x%02x' % (int_type,))
    int_val = struct.unpack_from(fmt, buf, pos + 1)[0]
    return (int_val, pos + needed)


def _bunser_string(buf, pos):
    str_len, pos = _bunser_int(buf, pos + 1)
    str_val = struct.unpack_from(str(str_len) + 's', buf, pos)[0]
    return (str_val, pos + str_len)


def _bunser_array(buf, pos, mutable=True):
    arr_len, pos = _bunser_int(buf, pos + 1)
    arr = []
    for i in range(arr_len):
        arr_item, pos = _bser_loads_recursive(buf, pos, mutable)
        arr.append(arr_item)

    if not mutable:
      arr = tuple(arr)

    return arr, pos


# This is a quack-alike with the bserObjectType in bser.c
# It offers the mapping protocol via the dict base class and
# provides sequence style access via integer index as well
# as getattr
class _BunserDict(object):
    __slots__ = ('_keys', '_values')

    def __init__(self, keys, values):
        self._keys = keys
        self._values = values

    def __getattr__(self, name):
        try:
            return self._values[self._keys.index(name)]
        except ValueError as ex:
            raise AttributeError('_BunserDict has no attribute %s' % name)

    def __getitem__(self, key):
        if isinstance(key, (int, long)):
            return self._values[key]
        elif key.startswith('st_'):
            # hack^Wfeature to allow mercurial to use "st_size" to
            # reference "size"
            key = key[3:]
        try:
            return self._values[self._keys.index(key)]
        except ValueError as ex:
            raise KeyError('_BunserDict has no key %s' % key)

    def __len__(self):
        print('doing len, have `%s` `%s`' % (self._keys, self._values))
        return len(self._keys)

def _bunser_object(buf, pos, mutable=True):
    obj_len, pos = _bunser_int(buf, pos + 1)
    if mutable:
        obj = {}
    else:
        keys = []
        vals = []

    for i in range(obj_len):
        key, pos = _bunser_string(buf, pos)
        val, pos = _bser_loads_recursive(buf, pos, mutable)
        if mutable:
            obj[key] = val
        else:
            keys.append(key)
            vals.append(val)

    if not mutable:
        obj = _BunserDict(keys, vals)

    return obj, pos


def _bunser_template(buf, pos, mutable=True):
    if buf[pos + 1] != BSER_ARRAY:
        raise RuntimeError('Expect ARRAY to follow TEMPLATE')
    keys, pos = _bunser_array(buf, pos + 1)
    nitems, pos = _bunser_int(buf, pos)
    arr = []
    for i in range(nitems):
        if mutable:
            obj = {}
        else:
            vals = []

        for keyidx in range(len(keys)):
            if buf[pos] == BSER_SKIP:
                pos += 1
                ele = None
            else:
                ele, pos = _bser_loads_recursive(buf, pos, mutable)

            if mutable:
                key = keys[keyidx]
                obj[key] = ele
            else:
                vals.append(ele)

        if not mutable:
            obj = _BunserDict(keys, vals)

        arr.append(obj)
    return arr, pos


def _bser_loads_recursive(buf, pos, mutable=True):
    val_type = buf[pos]
    if (val_type == BSER_INT8 or val_type == BSER_INT16 or
        val_type == BSER_INT32 or val_type == BSER_INT64):
        return _bunser_int(buf, pos)
    elif val_type == BSER_REAL:
        val = struct.unpack_from('=d', buf, pos + 1)[0]
        return (val, pos + 9)
    elif val_type == BSER_TRUE:
        return (True, pos + 1)
    elif val_type == BSER_FALSE:
        return (False, pos + 1)
    elif val_type == BSER_NULL:
        return (None, pos + 1)
    elif val_type == BSER_STRING:
        return _bunser_string(buf, pos)
    elif val_type == BSER_ARRAY:
        return _bunser_array(buf, pos, mutable)
    elif val_type == BSER_OBJECT:
        return _bunser_object(buf, pos, mutable)
    elif val_type == BSER_TEMPLATE:
        return _bunser_template(buf, pos, mutable)
    else:
        raise RuntimeError('unhandled bser opcode 0x%02x' % (val_type,))


def pdu_len(buf):
    if buf[0:2] != EMPTY_HEADER[0:2]:
        raise RuntimeError('Invalid BSER header')
    expected_len, pos = _bunser_int(buf, 2)
    return expected_len + pos


def loads(buf, mutable=True):
    if buf[0:2] != EMPTY_HEADER[0:2]:
        raise RuntimeError('Invalid BSER header')
    expected_len, pos = _bunser_int(buf, 2)
    if len(buf) != expected_len + pos:
        raise RuntimeError('bser data len != header len')
    return _bser_loads_recursive(buf, pos, mutable)[0]
