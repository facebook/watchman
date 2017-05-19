#!/bin/bash
set -x
case `uname` in
  Darwin)
    brew update
    brew tap facebook/fb
    brew install wget pcre ruby openssl readline buck pyenv
    # reinstall to get the latest versions
    brew upgrade
    # avoid snafu with OS X and python builds
    ARCHFLAGS=-Wno-error=unused-command-line-argument-hard-error-in-future
    CFLAGS="$CFLAGS $ARCHFLAGS"
    export ARCHFLAGS CFLAGS
    case "$TRAVIS_PYTHON" in
      python2.6)
        pyenv install 2.6.9
        eval "$(pyenv init -)"
        pyenv global 2.6.9
        # install some other required dependencies
        pip2.6 install unittest2==1.1.0 argparse
        ;;
      python2.7)
        pyenv install 2.7.11
        pyenv global 2.7.11
        ;;
      python3.5)
        pyenv install 3.5.2
        pyenv global 3.5.2
        ;;
      python3.6)
        pyenv install 3.6.1
        pyenv global 3.6.1
    esac
    ;;
esac
