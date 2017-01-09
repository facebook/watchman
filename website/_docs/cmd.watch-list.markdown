---
pageid: cmd.watch-list
title: watch-list
layout: docs
section: Commands
permalink: docs/cmd/watch-list.html
---

Returns a list of watched dirs.

From the command line:

```bash
$ watchman watch-list
```

JSON:

```json
["watch-list"]
```

Result:

```json
{
    "version": "1.9",
    "roots": [
        "/home/wez/watchman"
    ]
}
```
