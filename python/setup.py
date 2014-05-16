#!/usr/bin/env python
# vim:ts=4:sw=4:et:
from distutils.core import setup, Extension

setup(
    name = 'pywatchman',
    version = '0.1',
    description = 'Watchman client for python',
    author = 'Wez Furlong, Siddharth Agarwal',
    maintainer = 'Wez Furlong',
    maintainer_email = 'wez@fb.com',
    url = 'https://github.com/facebook/watchman',
    long_description = 'Connect and query Watchman to discover file changes',
    keywords = \
        'watchman inotify fsevents kevent kqueue portfs filesystem watcher',
    license = 'Apache 2.0',
    packages = ['pywatchman'],
    ext_modules = [
        Extension('pywatchman.bser', sources = ['pywatchman/bser.c'])
    ]
)

