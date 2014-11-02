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
if ! arc unit --everything ; then
  cat /tmp/watchman*
  exit 1
fi

if false ; then
  # Run the hgwatchman test suite too
  HERE=`pwd`
  cd a/hgwatchman/tests
  rm -rf /tmp/watchman-tmp
  mkdir /tmp/watchman-tmp
  PATH="$HERE:$PATH"
  TMPDIR=/tmp/watchman-tmp
  export PATH TMPDIR

  # Make sure that we're running our instance
  watchman --no-spawn shutdown-server
  # Start it up and skip the state file
  $HERE/watchman --no-save-state watch-list

  if ! TMPDIR=/tmp/watchman-tmp PATH="$HERE:$PATH" ./run-tests.py --hg=$HERE/a/hg -j 1 ; then
    $HERE/watchman --no-spawn shutdown-server
    cat $TMPDIR/.watchman.*.log
    echo "hgwatchman tests failed"
    exit 1
  fi
  cd $HERE
  $HERE/watchman --no-spawn shutdown-server
fi

INST_TEST=/tmp/install-test
test -d $INST_TEST && rm -rf $INST_TEST
make DESTDIR=$INST_TEST install
find $INST_TEST

exit 0

