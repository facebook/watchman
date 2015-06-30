---
id: release-notes
title: Release Notes
layout: docs
category: Installation
permalink: docs/release-notes.html
---

We focus on the highlights only in these release notes.  For a full history
that includes all of the gory details, please see [the commit history on GitHub](
https://github.com/facebook/watchman/commits/master).

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
