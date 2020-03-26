#!/bin/bash -e
# vim:ts=2:sw=2:et:
set -x
python3 build/fbcode_builder/getdeps.py build --src-dir=. watchman
python3 build/fbcode_builder/getdeps.py fixup-dyn-deps --src-dir=. watchman built --final-install-prefix /usr/local
find built -ls
