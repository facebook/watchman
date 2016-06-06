#!/bin/sh
set -x
uname -a

# Speculatively fulfil any node deps before we turn on hard errors
cd node
npm install
cd ..

set -e
PATH=$PWD:$PATH

if [ "$(uname)" = "Darwin" ]; then
  eval "$(pyenv init -)"
fi

./autogen.sh
./configure --with-pcre --with-python="$TRAVIS_PYTHON" --without-ruby $CONFIGARGS
make clean
make
set +e
rm -rf /tmp/watchman*
rm -rf /var/tmp/watchmantest*
TMP=/var/tmp
if test "$CIRCLECI" == "true" ; then
  TMP=$CIRCLE_ARTIFACTS
fi
TMPDIR=$TMP
export TMPDIR TMP

if ! make integration ; then
  if test "$CIRCLECI" == "true" ; then
    # runtests.py already copied the logs to the artifact store
    exit 1
  fi
  find /var/tmp/watchmantest* -name log | xargs cat
  exit 1
fi

INST_TEST=/tmp/install-test
test -d $INST_TEST && rm -rf $INST_TEST
make DESTDIR=$INST_TEST install
find $INST_TEST

chmod 2755 /tmp/install-test
stat /tmp/install-test

exit 0

