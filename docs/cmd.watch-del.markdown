---
id: cmd.watch-del
title: watch-del
layout: docs
category: Commands
permalink: docs/cmd/watch-del.html
---

Removes a watch.

From the command line:

```bash
$ watchman watch-del /path/to/dir
```

JSON:

```json
["watch-del", "/path/to/dir"]
```

Unless `no-state-save` is in use, the removed watch will also be removed
from the state file and will not be automatically watched if/when watchman
is restarted.
