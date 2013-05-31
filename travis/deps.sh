#!/bin/sh
set -x
case `uname` in
  Linux)
    sudo sysctl -A | grep inotify
    sudo apt-get install -y valgrind -q
    mount
    ;;
esac
set -e
if [ ! -d a ] ; then
  mkdir a
fi
cd a
rm -f master.zip
if [ ! -d libphutil ] ; then
  wget https://github.com/facebook/libphutil/archive/master.zip
  unzip master.zip
  mv libphutil-master libphutil
  rm master.zip
fi
if [ ! -d arcanist ] ; then
  wget https://github.com/facebook/arcanist/archive/master.zip
  unzip master.zip
  mv arcanist-master arcanist
  rm master.zip
fi
cd ..
ln -sf ./a/arcanist/bin/arc arc

