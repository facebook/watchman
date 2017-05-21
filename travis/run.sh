#!/bin/sh
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

if [ "${ENABLE_ASAN-0}" -eq 1 ]; then
  CONFIGARGS="--enable-asan --disable-opt"
  export CONFIGARGS
fi

./autogen.sh
./configure --with-pcre --with-python="$TRAVIS_PYTHON" --without-ruby $CONFIGARGS
make clean
make
set +e
rm -rf /tmp/watchman*
rm -rf /var/tmp/watchmantest*
TMP=/var/tmp
TMPDIR=$TMP
export TMPDIR TMP

if ! make integration ; then
  exit 1
fi

INST_TEST=/tmp/install-test
if test "$CIRCLE" = "true" ; then
  INST_TEST="$CIRCLE_ARTIFACTS/install-test"
fi
test -d $INST_TEST && rm -rf $INST_TEST
make DESTDIR=$INST_TEST install
find $INST_TEST

case $(uname) in
  Darwin)
    if [[ "$BUILD_JAVA_CLIENT" != "" ]] && [ "${BUILD_JAVA_CLIENT-0}" -eq 1 ]; then
      pushd java
      buck fetch :watchman :watchman-tests || exit 1
      buck test :watchman-lib || exit 1
      popd
    fi
    ;;
esac

exit 0

