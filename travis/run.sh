#!/bin/sh
set -x
uname -a
set -e
PATH=$PWD:$PATH
./autogen.sh
./configure --with-pcre --with-python $CONFIGARGS
make
cd ruby/ruby-watchman
bundle install && bundle exec rake
cd ../..
set +e
rm -f /tmp/watchman*
if ! arc unit --everything ; then
  cat /tmp/watchman*
  exit 1
fi
exit 0

