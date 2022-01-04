#!/bin/bash -e
# vim:ts=2:sw=2:et:
# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# This source code is licensed under the MIT license found in the
# LICENSE file in the root directory of this source tree.

set -x
PREFIX=${PREFIX:-/usr/local}
python3 build/fbcode_builder/getdeps.py build --src-dir=. watchman \
  "--project-install-prefix=watchman:$PREFIX"
python3 build/fbcode_builder/getdeps.py fixup-dyn-deps \
  --src-dir=. watchman  built \
  "--project-install-prefix=watchman:$PREFIX" \
  --final-install-prefix "$PREFIX"

find built -ls
