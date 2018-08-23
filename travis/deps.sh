#!/bin/bash
set -x

brew_install_latest_stable() {
  local packages=("${@}")
  set +x
  local packages_to_install=()
  local packages_to_upgrade=()
  for package in "${packages[@]}"; do
    case "$(brew_package_installed_status "${package}")" in
      not_installed)
        packages_to_install+=("${package}")
        ;;
      installed_but_outdated)
        packages_to_upgrade+=("${package}")
        ;;
      installed_and_up_to_date)
        ;;
      *)
        printf 'error: unknown package installed status\n' >&2
        return 1
        ;;
    esac
  done
  set -x
  if [ "${#packages_to_upgrade[@]}" -gt 0 ]; then
    brew upgrade "${packages_to_upgrade[@]}"
  fi
  if [ "${#packages_to_install[@]}" -gt 0 ]; then
    brew install "${packages_to_install[@]}"
  fi
}

brew_package_installed_status() {
  local package="${1}"
  if ! brew_package_is_installed "${package}"; then
    echo not_installed
    return
  fi
  local outdated_packages=($(brew outdated --quiet))
  for outdated_package in "${outdated_packages[@]:+${outdated_packages[@]}}"; do
    if [ "${package}" = "${outdated_package}" ]; then
      echo installed_but_outdated
      return
    fi
  done
  echo installed_and_up_to_date
}

brew_package_is_installed() {
  local package="${1}"
  brew list --versions "${package}" >/dev/null
}

case `uname` in
  Darwin)
    brew update >/dev/null
    HOMEBREW_NO_AUTO_UPDATE=1 brew_install_latest_stable cmake wget pcre ruby openssl readline pyenv double-conversion glog gflags boost libevent xz snappy lz4 pkg-config
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
