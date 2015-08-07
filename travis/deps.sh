#!/bin/bash
set -x
case `uname` in
  Linux)
#    sudo apt-get update -y -q
#    sudo apt-get install -y ruby rubygems valgrind php5-cli php5-curl mercurial -q
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
