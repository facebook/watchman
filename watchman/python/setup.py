#!/usr/bin/env python
# vim:ts=4:sw=4:et:
# Copyright (c) Facebook, Inc. and its affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

from distutils.core import setup, Extension
from pathlib import Path
this_directory = Path(__file__).parent


def srcs(names):
    """Concatenate this directory path to each source name."""
    return [str(this_directory / n) for n in names]


setup(
    name="pywatchman",
    version="1.4.1",
    description="Connect and query Watchman to discover file changes",
    author="Wez Furlong, Rain",
    author_email="wez@fb.com",
    maintainer="Wez Furlong",
    maintainer_email="wez@fb.com",
    url="https://github.com/facebook/watchman",
    keywords=["watchman", "inotify", "fsevents", "kevent", "kqueue", "portfs", "filesystem", "watcher"],
    classifiers=[
        "Development Status :: 5 - Production/Stable",
        "Intended Audience :: Developers",
        "Topic :: System :: Filesystems",
        "License :: OSI Approved :: MIT License",
        "Operating System :: OS Independent",
        "Programming Language :: Python :: 2",
        "Programming Language :: Python :: 2.6",
        "Programming Language :: Python :: 2.7",
        "Programming Language :: Python :: 3",
        "Programming Language :: Python :: 3.5",
        "Programming Language :: Python :: 3.6",
    ],
    packages=["pywatchman"],
    package_dir={"": str(this_directory)},
    ext_modules=[Extension("pywatchman.bser", srcs(["pywatchman/bser.c"]))],
    scripts=srcs([
        "bin/watchman-make",
        "bin/watchman-replicate-subscription",
        "bin/watchman-wait",
    ]),
)
