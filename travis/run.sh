#!/bin/sh
set -x
uname -a
set -e
PATH=$PWD:$PATH
if [ -d /var/ramfs ] ; then
  TMPDIR=/var/ramfs
  TMP=/var/ramfs
  export TMPDIR TMP
fi
./autogen.sh
./configure --with-pcre
make
set +e
if ! arc unit tests/integration/movereadd.php tests/integration/rmroot.php ; then
  cat /tmp/watchman-test.log
  exit 1
fi
exit 0

