#!/bin/sh
set -x
uname -a
set -e
PATH=$PWD:$PATH
./autogen.sh
./configure --with-pcre --with-python --with-ruby $CONFIGARGS
make clean
make
set +e
rm -f /tmp/watchman*
if ! arc unit --everything ; then
  cat /tmp/watchman*
  exit 1
fi

INST_TEST=/tmp/install-test
test -d $INST_TEST && rm -rf $INST_TEST
make DESTDIR=$INST_TEST install
find $INST_TEST

exit 0

