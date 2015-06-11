#!/bin/bash
set -x
case `uname` in
  Linux)
    sudo apt-get update -y -q
    sudo apt-get install -y ruby rubygems valgrind php5-cli php5-curl mercurial -q
    ;;
  Darwin)
    brew update
    brew install wget pcre ruby mercurial
    # avoid snafu with OS X and python builds
    ARCHFLAGS=-Wno-error=unused-command-line-argument-hard-error-in-future
    CFLAGS="$CFLAGS $ARCHFLAGS"
    export ARCHFLAGS CFLAGS
    ;;
esac
set -e
#sudo gem install bundler
#cd ruby/ruby-watchman
#bundle
#cd ../..
if [ ! -d a ] ; then
  mkdir a
fi
cd a
if [ ! -d libphutil ] ; then
  wget https://github.com/facebook/libphutil/archive/92882eb9404da16ba2ea498c1182cd3c1278877f.zip
  unzip 92882eb9404da16ba2ea498c1182cd3c1278877f.zip
  mv libphutil-92882eb9404da16ba2ea498c1182cd3c1278877f libphutil
  rm 92882eb9404da16ba2ea498c1182cd3c1278877f.zip
fi
if [ ! -d arcanist ] ; then
  wget https://github.com/facebook/arcanist/archive/e1a051a033b8b190383b1081b086a737621b8682.zip
  unzip e1a051a033b8b190383b1081b086a737621b8682.zip
  mv arcanist-e1a051a033b8b190383b1081b086a737621b8682 arcanist
  rm e1a051a033b8b190383b1081b086a737621b8682.zip
fi
if false ; then
if [ ! -d hg ] ; then
  hg clone -u stable http://selenic.com/hg
  cd hg
  make local
  cd ..
fi
if [ ! -d hgwatchman ] ; then
  hg clone https://bitbucket.org/facebook/hgwatchman
  cd hgwatchman
  make local
  cd ..
fi
fi
cd ..
ln -sf ./a/arcanist/bin/arc arc
