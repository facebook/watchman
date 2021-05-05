#!/usr/bin/env python
# vim:ts=4:sw=4:et:

from __future__ import absolute_import, division, print_function, unicode_literals

import os

try:
    from setuptools import setup, Extension
except ImportError:
    from distutils.core import setup, Extension

watchman_src_dir = os.environ.get("CMAKE_CURRENT_SOURCE_DIR")
if watchman_src_dir is None:
    watchman_src_dir = os.path.join(os.path.dirname(os.path.realpath(__file__)), "..")

# The python source dir.
# On Windows, this has to be relative to the cwd otherwise something
# in the setuptools machinery does the wrong thing and produces a
# path like `Z:blah` which on windows resolves ambiguously depending
# on the cwd.
py_dir = os.path.join(watchman_src_dir, "python")
if os.name == "nt":
    py_dir = os.path.relpath(py_dir)


def srcs(names):
    """transform a list of sources to be relative to py_dir"""
    return ["%s/%s" % (py_dir, n) for n in names]


setup(
    name="pywatchman",
    version="1.4.1",
    package_dir={"": py_dir},
    description="Watchman client for python",
    author="Wez Furlong, Rain",
    author_email="wez@fb.com",
    maintainer="Wez Furlong",
    maintainer_email="wez@fb.com",
    url="https://github.com/facebook/watchman",
    long_description="Connect and query Watchman to discover file changes",
    keywords=("watchman inotify fsevents kevent kqueue portfs filesystem watcher"),
    license="BSD",
    packages=["pywatchman"],
    ext_modules=[Extension("pywatchman.bser", sources=srcs(["pywatchman/bser.c"]))],
    platforms="Platform Independent",
    classifiers=[
        "Development Status :: 5 - Production/Stable",
        "Intended Audience :: Developers",
        "Topic :: System :: Filesystems",
        "License :: OSI Approved :: BSD License",
        "Operating System :: OS Independent",
        "Programming Language :: Python :: 2",
        "Programming Language :: Python :: 2.6",
        "Programming Language :: Python :: 2.7",
        "Programming Language :: Python :: 3",
        "Programming Language :: Python :: 3.5",
        "Programming Language :: Python :: 3.6",
    ],
    zip_safe=True,
    scripts=srcs(
        [
            "bin/watchman-make",
            "bin/watchman-wait",
            "bin/watchman-replicate-subscription",
        ]
    ),
    test_suite="tests",
)
