---
id: cmd.sync
title: sync
layout: docs
category: Commands
permalink: docs/cmd/sync.html
---

Syncs all pending events from the OS and returns a clock value representing
the most recent state of the watched root.

This is similar to 'clock' but it takes more care in updating the returned
value to be as recent as possible, but is a bit slower.


```bash
$ watchman sync /path/to/dir
```

JSON:

```json
["sync", "/path/to/dir"]
```
