# Copyright (c) Meta Platforms, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

# no unicode literals

import sys


"""Compatibility module across Python 2 and 3."""


PYTHON2 = sys.version_info < (3, 0)
PYTHON3 = sys.version_info >= (3, 0)

# This is adapted from https://bitbucket.org/gutworth/six, and used under the
# MIT license. See LICENSE for a full copyright notice.
if PYTHON3:

    def reraise(tp, value, tb=None):
        try:
            if value is None:
                value = tp()
            if value.__traceback__ is not tb:
                raise value.with_traceback(tb)
            raise value
        finally:
            value = None
            tb = None

    import collections.abc as abc
else:
    exec(
        """
def reraise(tp, value, tb=None):
    try:
        raise tp, value, tb
    finally:
        tb = None
""".strip()
    )

    import collections as abc

if PYTHON3:
    UNICODE = str
else:
    UNICODE = unicode  # noqa: F821 We handled versioning above

collections_abc = abc
