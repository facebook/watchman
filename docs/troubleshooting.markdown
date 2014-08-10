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
