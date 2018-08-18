#!/bin/bash -e
# vim:ts=2:sw=2:et:

if test ! -d external ; then
  ./getdeps.py --install-deps
fi

cmake .
make -j4

