#!/bin/sh
set -x
uname -a
set -e
PATH=$PWD:$PATH
./autogen.sh
./configure --with-pcre $CONFIGARGS
make
set +e
if ! arc unit --everything ; then
  cat /tmp/watchman-test.log
  exit 1
fi
exit 0

