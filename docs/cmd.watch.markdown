---
id: cmd.watch
title: watch
layout: docs
category: Commands
permalink: docs/cmd/watch.html
---

Requests that the specified dir is watched for changes.
Watchman will track all files and dirs rooted at the specified path.

From the command line:

```bash
$ watchman watch ~/www
```

Note that, when you're using the CLI, you can specify the root as `~/www`
because the shell will resolve `~/www` to `/home/wez/www`, but when you use the
JSON protocol, you are responsible for supplying an absolute path.

JSON:
```json
["watch", "/home/wez/www"]
```

Watchman will `realpath(3)` the directory and start watching it if it isn't
already.  A newly watched directory is processed in a couple of stages:

 * Establishes change notification for the directory with the kernel
 * Queues up a request to crawl the directory
 * As the directory contents are resolved, those are watched in a similar
   fashion
 * All newly observed files are considered changed

Unless `no-state-save` is in use, watches are saved and re-established across
a process restart.
