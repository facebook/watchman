---
id: troubleshooting
title: Troubleshooting
layout: docs
category: Troubleshooting
permalink: docs/troubleshooting.html
---

If you've been directed to this page due to an error or warning output from
Watchman, it typically means that there is some system tuning that you need to
perform.

## Recrawl

A recrawl is an action that Watchman performs in order to recover from
situations where it believes that it has lost sync with the state of the
filesystem.

The most common cause for a recrawl is on Linux systems where the default
inotify limits are sized quite small.  What this means is that the rate at
which your watched roots are generating changes is higher than the kernel can
buffer and relay to the watchman service.  When this happens, the kernel
detects the overflow and signals `IN_Q_OVERFLOW`.  The recovery is to
recursively scan the root to make sure that we know what is really there and
re-sync with the notification stream.

Frequent recrawls are undesirable because they result in a potentially
expensive full tree crawl, which marks all files as changed and propagates this
status to clients which will in turn perform some action on the (likely
falsely) changed state of the majority of files.

### Avoiding Recrawls

There is no simple formula for setting your system limits; bigger is better but
comes at the cost of kernel memory to maintain the buffers.  You and/or your
system administrator should review the workload for your system and the
[System Specific Preparation Documentation](
/watchman/docs/install.html#system-specific-preparation) and raise your limits
accordingly.

### I've changed my limits, how can I clear the warning?

The warning will stick until you cancel the watch and reinstate it, or restart
the watchman process.  The simplest resolution is to run `watchman
shutdown-server` and re-establish your watch on your next watchman query.

## Where are the logs?

If you configured watchman using `--enable-statedir=<STATEDIR>` the
default location for logfile will be `<STATEDIR>/<USER>.log`, otherwise it
will be `<TMPDIR>/.watchman.<USER>.log`.

This location is overridden by the `--logfile` [Server Option](
/watchman/docs/cli-options.html#server-options).

[Quick note on default locations](
/watchman/docs/cli-options.html#quick-note-on-default-locations) explains what
we mean by `<TMPDIR>`, `<USER>` and so on.

## Poison: inotify_add_watch

```
A non-recoverable condition has triggered.  Watchman needs your help!
The triggering condition was at timestamp=1407695600: inotify-add-watch(/my/path) -> Cannot allocate memory
All requests will continue to fail with this message until you resolve
the underlying problem.  You will find more information on fixing this at
https://facebook.github.io/watchman/docs/troubleshooting.html#poison-inotify-add-watch
```

If you've encountered this state it means that your *kernel* was unable to
watch a dir in one or more of the roots you've asked it to watch.  This
particular condition is considered non-recoverable by Watchman on the basis
that nothing that the Watchman service can do can guarantee that the root cause
is resolved, and while the system is in this state, Watchman cannot guarantee
that it can respond with the correct results that its clients depend upon.  We
consider ourselves poisoned and will fail all requests for all watches (not
just the watch that it triggered on) until the process is restarted.

There are two primary reasons that this can trigger:

* The user limit on the total number of inotify watches was reached or the
  kernel failed to allocate a needed resource
* Insufficient kernel memory was available

The resolution for the former is to revisit
[System Specific Preparation Documentation](
/watchman/docs/install.html#system-specific-preparation) and raise your limits
accordingly.

The latter condition implies that your workload is exceeding the available RAM
on the machine.  It is difficult to give specific advice to resolve this
condition here; you may be able to tune down other system limits to free up
some resources, or you may just need to install more RAM in the system.

### I've changed my limits, how can I clear the error?

The error will stick until you restart the watchman process.  The simplest
resolution is to run `watchman shutdown-server`.

If you have not actually resolved the root cause you may continue to trigger
and experience this state each time the system trips over these limits.
