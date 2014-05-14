---
id: cli-options
title: Command Line Options
layout: docs
category: Invocation
permalink: docs/cli-options.html
---

The following options are recognized if they are present on the command line
before any non-option arguments.

```bash
 -U, --sockname=PATH   Specify alternate sockname

 -o, --logfile=PATH    Specify path to logfile

 -p, --persistent      Persist and wait for further responses.
                       You should probably also use --server-encoding=json

 -n, --no-save-state   Don't save state between invocations

 --statefile=PATH      Specify path to file to hold watch and trigger state

 -f, --foreground      Run the service in the foreground

 -j, --json-command    Instead of parsing CLI arguments, take a single json
                       object from stdin

 --no-pretty           Don't pretty print the JSON response

 --no-spawn            Don't spawn service if it is not already running.
                       Will try running the command in client mode if possible.
```

 * See `log-level` for an example of when you might use the `persistent` flag

## Environment and Files

Watchman will use the `$USER` or `$LOGNAME` environmental variables to
determine the current userid, falling back to the information it resolves via
`getpwuid(getuid())`, and the `$TMPDIR` or `$TMP` environmental variables to
determine a temporary directory, falling back to `/tmp` if neither are set.

If you configured watchman using the `--enable-statedir` option:

 * The default `sockname` is `<STATEDIR>/$USER`
 * The default `logfile` is `<STATEDIR>/$USER.log`
 * The default `statefile` is `<STATEDIR>/$USER.state`.  You can turn off
   the use of the state file via the `--no-save-state` option.  The statefile
   is used to persist watches and triggers across process restarts.

Otherwise:

 * The default `sockname` is `$TMP/.watchman.$USER`
 * The default `logfile` is `$TMP/.watchman.$USER.log`
 * The default `statefile` is `$TMP/.watchman.$USER.state`.  You can turn off
   the use of the state file via the `--no-save-state` option.  The statefile
   is used to persist watches and triggers across process restarts.
