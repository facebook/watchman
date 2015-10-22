---
id: release-notes
title: Release Notes
layout: docs
section: Installation
permalink: docs/release-notes.html
---

We focus on the highlights only in these release notes.  For a full history
that includes all of the gory details, please see [the commit history on
GitHub](https://github.com/facebook/watchman/commits/master).

### pywatchman 1.3.0 (2015-10-22)

* Added `watchman-make` and `watchman-wait` commands
* Added pure python implementation of BSER

### Watchman 4.1.0 (2015-10-20)

* Fixed an issue where symlink size was always reported as 0 on OS X
  using the new bulkstat functionality

### Watchman 4.0.0 (2015-10-19)

* Fixed an issue where a directory that was replaced by a symlink would
  cause a symlink traversal instead of correctly updating the type of the
  node and marking the children removed.
* Fixed a debugging log line that was emitted at the wrong log level on
  every directory traversal.

### Watchman 3.9.0 (2015-10-12)

* Fixed an issue where dir renames on OS X could cause us to lose track of
  the files inside the renamed dir
* Fixed an issue where dir deletes and replacements on Linux could cause us
  to lose track of the files inside the replaced dir (similar to the OS X issue
  above in manifestation, but a different root cause).
* Improved (re)crawl speed for dirs with more than a couple of entries on average
  (improvement can be up to 5x for dirs with up to 64 entries on average).
  You may now tune the `hint_num_files_per_dir` setting in your
  `.watchmanconfig` to better match your tree.  [More details](
  /watchman/docs/config.html#hint_num_files_per_dir)
* Improved (re)crawl speed on OS X 10.10 and later by using `getattrlistbulk`.
  This allows us to improve the data:syscall ratio during crawling and can
  improve throughput by up to 40% for larger trees.
* Add optional `sync_timeout` to the `clock` command
* Avoid accidentally passing descriptors other than the stdio streams
  when we spawn the watchman service.
* Fixed a race condition where we could start two sets of watcher threads
  for the same dir if two clients issue a `watch` or `watch-project` at
  the same time
* Added a helpful error for a tmux + launchd issue on OS X

### Watchman 3.8.0 (2015-09-14)

* Improved latency of processing kernel notifications. It should now be far
  less likely to run into an notification queue overflow.
* Improved idle behavior. There were a couple of places where watchman would
  wake up more often than was strictly needed and these have now been fixed.
  This is mostly of interest to laptop users on battery power.
* Improved inotify move tracking.  Some move operations could cause watchman
  to become confused and trigger a recrawl.  This has now been resolved.
* Hardened statedir and permissions. There was a possibility of a symlink
  attack and this has now been mitigated by re-structuring the statedir layout.
* Fixed a possible deadlock in the idle watch reaper
* Fixed an issue where the watchman -p log-level debug could drop log
  notifications in the CLI
* Disabled the IO-throttling-during-crawl that we added in 3.7. It proved to
  be more harmful than beneficial.
* `-j` CLI option now accepts either JSON or BSER encoded command on stdin
* Added [capabilities](/watchman/docs/capabilities.html) to the server,
  and added the [capabilityCheck](/watchman/docs/cmd/version.html#capabilityCheck)
  method to the python and node clients.

### pywatchman 1.2.0 (2015-08-15)

* Added the `capabilityCheck` method
* Added `SocketTimeout` exception to distinguish timeouts from protocol level
  exceptions

### fb-watchman 1.3.0 for node (2015-08-15)

* Added the [capabilityCheck](/watchman/docs/nodejs.html#checking-for-watchman-availability) method.

### pywatchman 1.0.0 (2015-08-06)

* First official pypi release, thanks to [@kwlzn](https://github.com/kwlzn)
  for setting up the release machinery for this.

### Watchman 3.7.0 (2015-08-05)

(Watchman 3.6.0 wasn't formally released)

* Fixed bug where `query match` on `foo*.java` with `wholename` scope
  would incorrectly match `foo/bar/baz.java`.
* Added `src/**/*.java` recursive glob pattern support to `query match`.
* Added options dictionary to `query`'s `match` operator.
* Added `includedotfiles` option to `query match` to include files
  whose names start with `.`.
* Added `noescape` option to `query match` to make `\` match literal `\`.
* We'll now automatically age out and stop watches. See [idle_reap_age_seconds](
/watchman/docs/config.html#idle_reap_age_seconds) for more information.
* `watch-project` will now try harder to re-use an existing watch and avoid
  creating an overlapping watch.
* Reduce I/O priority during crawling on systems that support this
* Fixed issue with the `long long` data type in the python BSER module

### fb-watchman 1.2.0 for node (2015-07-11)

* Updated the node client to more gracefully handle `undefined` values in
  objects when serializing them; we now omit keys whose values are `undefined`
  rather than throw an exception.

### Watchman 3.5.0 (2015-06-29)

* Fix the version number reported by watchman.

### Watchman 3.4.0 (2015-06-29)

* `trigger` now supports an optional `relative_root` argument. The trigger is
  evaluated with respect to this subdirectory. See
  [trigger](/watchman/docs/cmd/trigger.html#relative-roots) for more.

### fb-watchman 1.1.0 for node (2015-06-25)

* Updated the node client to handle 64-bit integer values using the
  [node-int64](https://www.npmjs.com/package/node-int64).  These are most
  likely to show up if your query fields include `size` and you have files
  larger than 2GB in your watched root.

### fb-watchman 1.0.0 for node (2015-06-23)

* Updated the node client to support [BSER](/watchman/docs/bser.html)
  encoding, fixing a quadratic performance issue in the JSON stream
  decoder that was used previously.

### Watchman 3.3.0 (2015-06-22)

* `query` and `subscribe` now support an optional `relative_root`
  argument. Inputs and outputs are evaluated with respect to this
  subdirectory. See
  [File Queries](/watchman/docs/file-query.html#relative-roots) for more.
