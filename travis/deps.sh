#!/bin/bash
set -ex

case "$OSTYPE" in
  darwin*)
    # Ensure that we pick up the homebrew openssl installation
    export PKG_CONFIG_PATH="$(brew --prefix)/opt/openssl/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
    brew update >/dev/null
    # getdeps.py installs deps for the watchman service, but we do try to test
    # our nodejs at runtime.  The boost dependency of folly causes icu4c to be
    # installed and there appears to be some version weirdo wrt. node and icu4c,
    # so let's force nodejs to be reinstalled and see if that helps.
    brew install nodejs || brew upgrade nodejs || true

    # avoid snafu with OS X and python builds
    #ARCHFLAGS=-Wno-error=unused-command-line-argument-hard-error-in-future
    #CFLAGS="$CFLAGS $ARCHFLAGS"
    #export ARCHFLAGS CFLAGS
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

    HOMEBREW_NO_AUTO_UPDATE=1 ./getdeps.py --install-deps
    ;;
  *)
    ./getdeps.py
    ;;
esac
cmake .
