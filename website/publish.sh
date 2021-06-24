#!/bin/bash

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
