#!/bin/bash -e
# vim:ts=2:sw=2:et:
set -x

case "$OSTYPE" in
  darwin*)
    # Ensure that we pick up the homebrew openssl installation
    if command -v brew >/dev/null ; then
      export PKG_CONFIG_PATH="$(brew --prefix)/opt/openssl/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
    fi
    ;;
esac

if test ! -d external ; then
  ./getdeps.py --install-deps
fi

cmake .
