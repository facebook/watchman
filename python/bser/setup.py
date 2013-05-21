#!/usr/bin/env python
# vim:ts=4:sw=4:et:
from distutils.core import setup, Extension

setup(
    name = 'bser',
    version = '0.1',
    description = 'BSER serialization for Python',
    author = 'Wez Furlong',
    ext_modules = [
        Extension('bser', sources = ['bser.c'])
    ]
)

