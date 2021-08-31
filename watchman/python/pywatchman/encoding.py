# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# no unicode literals
from __future__ import absolute_import, division, print_function

import sys

from . import compat


"""Module to deal with filename encoding on the local system, as returned by
Watchman."""


if compat.PYTHON3:
    default_local_errors = "surrogateescape"

    def get_local_encoding():
        if sys.platform == "win32":
            # Watchman always returns UTF-8 encoded strings on Windows.
            return "utf-8"
        # On the Python 3 versions we support, sys.getfilesystemencoding never
        # returns None.
        return sys.getfilesystemencoding()


else:
    # Python 2 doesn't support surrogateescape, so use 'strict' by
    # default. Users can register a custom surrogateescape error handler and use
    # that if they so desire.
    default_local_errors = "strict"

    def get_local_encoding():
        if sys.platform == "win32":
            # Watchman always returns UTF-8 encoded strings on Windows.
            return "utf-8"
        fsencoding = sys.getfilesystemencoding()
        if fsencoding is None:
            # This is very unlikely to happen, but if it does, just use UTF-8
            fsencoding = "utf-8"
        return fsencoding


def encode_local(s):
    return s.encode(get_local_encoding(), default_local_errors)


def decode_local(bs):
    return bs.decode(get_local_encoding(), default_local_errors)
