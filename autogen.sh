#!/bin/bash -e
# vim:ts=2:sw=2:et:
set -x
PREFIX=${PREFIX:-/usr/local}
python3 build/fbcode_builder/getdeps.py build --src-dir=. watchman \
  "--project-install-prefix=watchman:$PREFIX"
python3 build/fbcode_builder/getdeps.py fixup-dyn-deps \
  --src-dir=. watchman  built \
  "--project-install-prefix=watchman:$PREFIX" \
  --final-install-prefix "$PREFIX"

find built -ls
