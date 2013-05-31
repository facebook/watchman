#!/bin/sh
set -x
case `uname` in
  Linux)
    sudo sysctl -w fs.inotify.max_user_instances=1024
    sudo sysctl -w fs.inotify.max_user_watches=1000000
    sudo sysctl -w fs.inotify.max_queued_events=16384
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
