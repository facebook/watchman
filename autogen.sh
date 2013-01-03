#!/bin/sh
# vim:ts=2:sw=2:et:
if test -d autom4te.cache ; then
  rm -rf autom4te.cache
fi
aclocal
autoheader
automake --add-missing --foreign
autoconf
