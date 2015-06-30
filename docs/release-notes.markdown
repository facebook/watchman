---
id: release-notes
title: Release Notes
layout: docs
category: Installation
permalink: docs/release-notes.html
---

### Watchman 3.5.0 (2015-06-29)

* Fix the version number reported by watchman.

### Watchman 3.4.0 (2015-06-29)

* `trigger` now supports an optional `relative_root` argument. The trigger is
  evaluated with respect to this subdirectory. See
  [trigger](/watchman/docs/cmd/trigger.html#relative-roots) for more.

### Watchman 3.3.0 (2015-06-22)

* `query` and `subscribe` now support an optional `relative_root`
  argument. Inputs and outputs are evaluated with respect to this
  subdirectory. See
  [File Queries](/watchman/docs/file-query.html#relative-roots) for more.
