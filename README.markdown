# Watchman

A file watching service.

## Purpose

Watchman exists to watch files and record when they actually change.  It can
also trigger actions (such as rebuilding assets) when matching files change.

## Usage

Currently pretty simplistic; watchman understands the commands described below.
Where you see `[patterns]` in the command syntax, we allow filename patterns
that match according the following rules:

### Pattern syntax

 * We support `fnmatch(3)` glob style pattern matches.
 * A `!` followed by space followed by a pattern will negate the sense of the
   pattern match, so `! *.js` matches any file(s) that do not match the glob
   `*.js`.
 * A `-X` switches the pattern parser into exclusion mode.  Exclusion mode
   remains enabled until it is turned off by `-I`.  Patterns that parsed
   in exclusion mode will cause matched files to be excluded from the set
   of files that we return in the match results.  This can be useful to
   specify a set of files that you don't care about at the start of your
   pattern set, then later on use `-I` to indicate the set that you do
   want to include.
 * A `-I` switches the pattern parser into inclusion mode, which is the
   default.
 * A `--` indicates the end of the set of patterns.

### watchman watch /path/to/dir

Requests that the specified dir is watched for changes.
Watchman will track all files and dirs rooted at the specified path.

### watchman trigger /path/to/dir [patterns] -- [cmd]

Sets up a trigger such that if any files under the specified dir that match the
specified set of patterns change, then the specified command will be run and
passed the list of matching files.

For example:

    $ watchman trigger ~/www '*.js' -- ls -l

If, say, `~/www/scripts/foo.js` is changed, then watchman will chdir
to `~/www` then invoke `ls -l scripts/foo.js`.

Watchman waits for the filesystem to settle before processing any
triggers, batching the list of changed files together before invoking
the registered command.

Deleted files are counted as changed files and are passed the command in
exactly the same way.

### watchman find /path/to/dir [patterns]

Finds all files that match the optional list of patterns under the
specified dir.  If no patterns were specified, all files are returned.

### watchman since /path/to/dir <timestamp> [patterns]

Finds all files that were modified since the specified timestamp that
match the optional list of patterns.  If no patterns are specified,
all modified files are returned.

## Implementation details

Watchman runs as a single long-lived server process per user.
The watchman command will take care of spawning the server process
if necessary.

The server process listens on a unix domain socket and processes
commands using a simple line based protocol.  More advanced tools
can talk to this socket directly.  You can also do it yourself:

    nc -U /tmp/.watchman.$USER

## Future

 * persist the registered command triggers across process invocations
 * add a command to remove registered triggers
 * expose the internal clock value to allow clients to track changes
   in a race-free manner
 * add a richer output mode for the `since` command that passes on
   file status information in addition to the filename.
 * add a hybrid of since and trigger that allows a connected client
   to receive notifications of file changes over their unix socket
   connection as they happen.  Disconnecting the client will disable
   those particular notifications.

## OpenSource

This project is destined to be opened up, so any contributions need
to be aware of that and should only pull in code with a compatible
license (Apache).

