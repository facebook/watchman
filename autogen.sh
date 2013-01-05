#!/bin/sh
# vim:ts=2:sw=2:et:
if test -d autom4te.cache ; then
  rm -rf autom4te.cache
fi
if test -d config.status ; then
  rm -f config.status
fi
aclocal
autoheader
automake --add-missing --foreign
autoconf
