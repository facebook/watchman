---
id: cmd.version
title: version
layout: docs
section: Commands
permalink: docs/cmd/version.html
---

The version command will tell you the version and build information
for the currently running watchman service:

```bash
$ watchman version
{
    "version": "2.9.6",
    "buildinfo": "git:2727d9a1e47a4a2229c65cbb2f0c7656cbd96270"
}
```

To get the version of the client:

```bash
$ watchman -v
2.9.8
```

If the server and client versions don't match up, you should probably
restart your server: `watchman shutdown-server ; watchman`.
