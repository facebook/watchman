#!/bin/sh
set -x
uname -a
set -e
PATH=$PWD:$PATH
./autogen.sh
./configure --with-pcre
make
set +e
if ! arc unit tests/integration/movereadd.php tests/integration/rmroot.php ; then
  cat /tmp/watchman-test.log
fi
exit 0

