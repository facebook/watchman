#!/bin/sh
set -x
uname -a
set -e
PATH=$PWD:$PATH
./autogen.sh
./configure --with-pcre --with-python --without-ruby $CONFIGARGS
make clean
make
set +e
rm -rf /tmp/watchman*
TMPDIR=/var/tmp
TMP=/var/tmp
export TMPDIR TMP
if ! make integration ; then
  cat /tmp/watchman*
  exit 1
fi

INST_TEST=/tmp/install-test
test -d $INST_TEST && rm -rf $INST_TEST
make DESTDIR=$INST_TEST install
find $INST_TEST

exit 0

