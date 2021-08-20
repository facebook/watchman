#!/bin/bash
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

# stop running if any of these steps fail
set -e
WATCHMAN=$(hg root)/fbcode/watchman

if test ! -d /tmp/watchman-gh-pages ; then
  git clone -b gh-pages git@github.com:facebook/watchman.git /tmp/watchman-gh-pages
fi
cd /tmp/watchman-gh-pages

git checkout -- .
git clean -dfx
git fetch
git rebase origin/gh-pages
cd $WATCHMAN/oss/website
bundle exec jekyll build -d /tmp/watchman-gh-pages
cd /tmp/watchman-gh-pages

git add --all
git commit -m "update website"
git push origin gh-pages
