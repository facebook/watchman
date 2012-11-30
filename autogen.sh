#!/bin/sh
# vim:ts=2:sw=2:et:
aclocal
autoheader
automake --add-missing --foreign
autoconf
