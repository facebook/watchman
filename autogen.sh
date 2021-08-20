#!/bin/bash -e
# vim:ts=2:sw=2:et:
# Copyright (c) Facebook, Inc. and its affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

set -x
PREFIX=${PREFIX:-/usr/local}
python3 build/fbcode_builder/getdeps.py build --src-dir=. watchman \
  "--project-install-prefix=watchman:$PREFIX"
python3 build/fbcode_builder/getdeps.py fixup-dyn-deps \
  --src-dir=. watchman  built \
  "--project-install-prefix=watchman:$PREFIX" \
  --final-install-prefix "$PREFIX"

find built -ls
