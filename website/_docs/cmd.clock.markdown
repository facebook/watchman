---
id: cmd.clock
title: clock
layout: docs
section: Commands
permalink: docs/cmd/clock.html
---

Returns the current clock value for a watched root.

Be careful how you interpret this value; it returns the instantaneous value of
the clock, and may have changed by the time you intend to act upon it.

```bash
$ watchman clock /path/to/dir
```

JSON:

```json
["clock", "/path/to/dir"]
```
