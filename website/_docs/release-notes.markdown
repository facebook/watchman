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

### Watchman 3.8.0 (Not yet released)

* `-j` CLI option now accepts either JSON or BSER encoded command on stdin

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
