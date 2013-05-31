#!/bin/sh
set -ex
PATH=$PWD:$PATH
./autogen.sh
./configure --with-pcre
make
arc unit --everything

