#!/bin/bash
set -x
case `uname` in
  Linux)
    sudo apt-get update -y -q
    sudo apt-get install -y ruby rubygems valgrind php5-cli php5-curl mercurial -q
    ;;
  Darwin)
    brew update
    brew install wget pcre mercurial
    # avoid snafu with OS X and python builds
    ARCHFLAGS=-Wno-error=unused-command-line-argument-hard-error-in-future
    CFLAGS="$CFLAGS $ARCHFLAGS"
    export ARCHFLAGS CFLAGS
    ;;
esac
set -e
sudo gem install bundler
if [ ! -d a ] ; then
  mkdir a
fi
cd a
rm -f master.zip
if [ ! -d libphutil ] ; then
  wget https://github.com/facebook/libphutil/archive/master.zip
  unzip master.zip
  mv libphutil-master libphutil
  rm master.zip
fi
if [ ! -d arcanist ] ; then
  wget https://github.com/facebook/arcanist/archive/master.zip
  unzip master.zip
  mv arcanist-master arcanist
  rm master.zip
fi
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
cd ..
ln -sf ./a/arcanist/bin/arc arc
