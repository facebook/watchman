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

    case "$TRAVIS_PYTHON" in
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

    #HOMEBREW_NO_AUTO_UPDATE=1 ./getdeps.py --install-deps
    ./build/fbcode_builder/getdeps.py build watchman
    ;;
  *)
    ./getdeps.py
    ;;
esac
cmake .
