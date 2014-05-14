---
id: install
title: Installation
layout: docs
category: Installation
permalink: docs/install.html
---

## System Requirements

Watchman is known to compile and pass its test suite on:

 * Linux systems with `inotify`
 * OS X (uses FSEvents)
 * BSDish systems (FreeBSD 9.1, OpenBSD 5.2) that have the
   `kqueue(2)` facility
 * Illumos and Solaris style systems that have `port_create(3C)`

Watchman relies on the operating system facilities for file notification,
which means that you will likely have very poor results using it on any
kind of remote or distributed filesystem.

Watchman does not currently support Windows or any other operating system
not covered by the list above.

## Build/Install

You can use these steps to get watchman built:

```bash
./autogen.sh
./configure
make
```

[![Build Status](https://travis-ci.org/facebook/watchman.png)](
https://travis-ci.org/facebook/watchman)

## System Specific Preparation

### Linux inotify Limits

The `inotify(7)` subsystem has three important tunings that impact watchman.

 * `/proc/sys/fs/inotify/max_user_instances` impacts how many different
   root dirs you can watch.
 * `/proc/sys/fs/inotify/max_user_watches` impacts how many dirs you
   can watch across all watched roots.
 * `/proc/sys/fs/inotify/max_queued_events` impacts how likely it is that
   your system will experience a notification overflow.

You obviously need to ensure that `max_user_instances` and `max_user_watches`
are set so that the system is capable of keeping track of your files.

`max_queued_events` is important to size correctly; if it is too small, the
kernel will drop events and watchman won't be able to report on them.  Making
this value bigger reduces the risk of this happening.

Watchman has two simple strategies for mitigating an overflow of
`max_queued_events`:

 * It uses a dedicated thread to consume kernel events as quickly as possible
 * When the kernel reports an overflow, watchman will assume that all the files
   have been modified and will re-crawl the directory tree as though it had just
   started watching the dir.

This means that if an overflow does occur, you won't miss a legitimate change
notification, but instead will get spurious notifications for files that
haven't actually changed.

### Max OS File Descriptor Limits

The default per-process descriptor limit on current versions of OS X is
extremely low (256!).  More recent versions of watchman (2.9.2 and later)
use FSEvents and are not so sensitive to descriptor limits.

Watchman will attempt to raise its descriptor limit to match
`kern.maxfilesperproc` when it starts up, so you shouldn't need to mess
with `ulimit`; just raising the sysctl should do the trick.

The following will raise the limits to allow 10 million files total, with 1
million files per process until your next reboot.

    sudo sysctl -w kern.maxfiles=10485760
    sudo sysctl -w kern.maxfilesperproc=1048576

Putting the following into a file named `/etc/sysctl.conf` on OS X will cause
these values to persist across reboots:

    kern.maxfiles=10485760
    kern.maxfilesperproc=1048576
