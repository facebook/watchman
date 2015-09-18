#!/usr/bin/env python
# vim:ts=4:sw=4:et:

try:
    from setuptools import setup, Extension
except:
    from distutils.core import setup, Extension

setup(
    name = 'pywatchman',
    version = '1.2.0',
    description = 'Watchman client for python',
    author = 'Wez Furlong, Siddharth Agarwal',
    author_email = 'wez@fb.com',
    maintainer = 'Wez Furlong',
    maintainer_email = 'wez@fb.com',
    url = 'https://github.com/facebook/watchman',
    long_description = 'Connect and query Watchman to discover file changes',
    keywords = (
        'watchman inotify fsevents kevent kqueue portfs filesystem watcher'
    ),
    license = 'Apache License (2.0)',
    packages = ['pywatchman'],
    ext_modules = [
        Extension('pywatchman.bser', sources = ['pywatchman/bser.c'])
    ],
    platforms = 'Platform Independent',
    classifiers = [
        'Development Status :: 5 - Production/Stable',
        'Topic :: System :: Filesystems',
        'License :: OSI Approved :: Apache Software License',
        'Operating System :: OS Independent'
    ],
    zip_safe = True
)
