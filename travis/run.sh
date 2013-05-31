#!/bin/sh
set -x
uname -a
cat /proc/cpuinfo
set -e
PATH=$PWD:$PATH
./autogen.sh
./configure --with-pcre
make
set +e
if ! arc unit --everything ; then
  cat /tmp/watchman-test.log
fi
exit 0

