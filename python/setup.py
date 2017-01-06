#!/usr/bin/env python
# vim:ts=4:sw=4:et:

try:
    from setuptools import setup, Extension
except:
    from distutils.core import setup, Extension

setup(
    name = 'pywatchman',
    version = '1.4.0',
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
    license = 'BSD',
    packages = ['pywatchman'],
    ext_modules = [
        Extension('pywatchman.bser', sources = ['pywatchman/bser.c'])
    ],
    platforms = 'Platform Independent',
    classifiers = [
        'Development Status :: 5 - Production/Stable',
        'Intended Audience :: Developers',
        'Topic :: System :: Filesystems',
        'License :: OSI Approved :: BSD License',
        'Operating System :: OS Independent',
        'Programming Language :: Python :: 2',
        'Programming Language :: Python :: 2.6',
        'Programming Language :: Python :: 2.7',
        'Programming Language :: Python :: 3',
        'Programming Language :: Python :: 3.5',
        'Programming Language :: Python :: 3.6',
    ],
    zip_safe = True,
    scripts=['bin/watchman-wait', 'bin/watchman-make'],
    test_suite = 'tests',
)
