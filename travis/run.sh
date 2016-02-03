#!/bin/sh
set -x
uname -a

# Speculatively fulfil any node deps before we turn on hard errors
cd node
npm install
cd ..

set -e
PATH=$PWD:$PATH
./autogen.sh
./configure --with-pcre --with-python --without-ruby $CONFIGARGS
make clean
make
set +e
rm -rf /tmp/watchman*
rm -rf /var/tmp/watchmantest*
TMPDIR=/var/tmp
TMP=/var/tmp
export TMPDIR TMP
if ! make integration ; then
  find /var/tmp/watchmantest* -name log | xargs cat
  exit 1
fi

INST_TEST=/tmp/install-test
test -d $INST_TEST && rm -rf $INST_TEST
make DESTDIR=$INST_TEST install
find $INST_TEST

exit 0

