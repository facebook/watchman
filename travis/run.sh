#!/bin/bash
set -x
uname -a

# Speculatively fulfil any node deps before we turn on hard errors
cd node
npm install
cd ..

set -e
PATH=$PWD:$PATH

case $(uname) in
  Darwin)
    eval "$(pyenv init -)"
    ;;
  Linux)
    if [ "${ENABLE_ASAN-0}" -eq 1 ]; then
      # Some versions of gcc-5 appear to have a bug where -fsanitize-address
      # doesn't automatically enable the gold linker.
      LDFLAGS="$LDFLAGS -fuse-ld=gold"
      export LDFLAGS
    fi
    ;;
esac

INST_TEST=/tmp/install-test
if test "$CIRCLE" = "true" ; then
  INST_TEST="$CIRCLE_ARTIFACTS/install-test"
fi

rm -rf /tmp/watchman* $INST_TEST
rm -rf /var/tmp/watchmantest*
TMP=/var/tmp
TMPDIR=$TMP
export TMPDIR TMP

set +e

WAT_SRC=$PWD
python getdeps.py -j2 || exit 1

if [ -n "${TRAVIS_PYTHON}" ]; then
  cmake "$WAT_SRC" -DPYTHON_EXECUTABLE="$(which $TRAVIS_PYTHON)"
else
  cmake "$WAT_SRC"
fi

make clean
make VERBOSE=1

if ! make integration VERBOSE=1 ; then
  exit 1
fi

make DESTDIR=$INST_TEST install
find $INST_TEST

case $(uname) in
  Darwin)
    if [[ "$BUILD_JAVA_CLIENT" != "" ]] && [ "${BUILD_JAVA_CLIENT-0}" -eq 1 ]; then
      pushd "$WAT_SRC/java"
      buck fetch :watchman :watchman-tests || exit 1
      buck test :watchman-lib || exit 1
      popd
    fi
    ;;
esac

exit 0

