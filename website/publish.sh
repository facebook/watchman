#!/bin/sh

# stop running if any of these steps fail
set -e

cd ../../watchman-gh-pages
git checkout -- .
git clean -dfx
git fetch
git rebase origin/gh-pages
rm -Rf *
cd ../watchman/website
node server/generate.js
cp -R build/watchman/* ../../watchman-gh-pages/
rm -Rf build/
cd ../../watchman-gh-pages
git add --all
git commit -m "update website"
git push origin gh-pages
cd ../watchman/website
