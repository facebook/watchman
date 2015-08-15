#!/bin/sh

# stop running if any of these steps fail
set -e
cd ../..

if test ! -d watchman-gh-pages ; then
  git clone -b gh-pages git@github.com:facebook/watchman.git watchman-gh-pages
fi
cd watchman-gh-pages

git checkout -- .
git clean -dfx
git fetch
git rebase origin/gh-pages
cd ../watchman/website
jekyll build -d ../../watchman-gh-pages
cd ../../watchman-gh-pages

git add --all
git commit -m "update website"
git push origin gh-pages
cd ../watchman/website
