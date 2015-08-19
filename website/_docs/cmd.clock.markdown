---
id: cmd.clock
title: clock
layout: docs
section: Commands
permalink: docs/cmd/clock.html
---

Returns the current clock value for a watched root.

If a sync_timeout option is supplied, the command blocks for the given
number of microseconds waiting for all pending changes to sync.  The
returned clock is then guaranteed to be up to date.

With no sync_timeout, it returns the instantaneous value of the clock,
which may be slightly out of date if all file events have not been
fully read.

```bash
$ watchman clock /path/to/dir
```

JSON:

Note the third options argument is optional.

```json
["clock", "/path/to/dir", {"sync_timeout": 100}]
```
