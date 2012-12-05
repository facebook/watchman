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

### watchman since /path/to/dir <clockspec> [patterns]

Finds all files that were modified since the specified clockspec that
match the optional list of patterns.  If no patterns are specified,
all modified files are returned.

### Clockspec

For commands that query based on time, watchman offers a couple of different
ways to measure time.

 * number of seconds since the unix epoch (unix `time_t` style)
 * clock id of the form `c:123:234`
 * a named cursor of the form `n:whatever`

The first and most obvious is passing a unix timestamp.  Watchman records
the observed time that files change and allows you to find file that have
changed since that time.  Using a timestamp is prone to race conditions
in understanding the complete state of the file tree.

Using an abstract clock id insulates the client from these race conditions by
ticking as changes are detected rather than as time moves.  Watchman returns
the current clock id when it delivers match results; you can use that value as
the clockspec in your next time relative query to get a race free assessment of
changed files.

As a convenience, watchman can maintain the last observed clock for a client by
associating it with a client defined cursor name.  For example, you could
enumerate all the "C" source files on your first invocation of:

```
watchman since /path/to/src n:c_srcs *.c
```

and when you run it a second time, it will show you only the "C" source files
that changed since the last time that someone queried using "n:c_srcs" as the
clock spec.

## Implementation details

Watchman runs as a single long-lived server process per user.
The watchman command will take care of spawning the server process
if necessary.

The server process listens on a unix domain socket and processes
commands using a simple line based protocol.  More advanced tools
can talk to this socket directly.  You can also do it yourself:

    nc -U /tmp/.watchman.$USER

### Streaming JSON protocol

Watchman uses a stream of JSON object or array values as its protocol.
Each request is a JSON array followed by a newline.
Each response is a JSON object followed by a newline.

By default, the protocol is request-response based, but some commands
can enable a mode wherein the server side can unilaterally decide to
send JSON object responses that contain log or trigger information.

Clients that enable these modes will need to be prepared to receive
one or more of these unilateral responses at any time.  Note that
the server will ensure that only complete JSON objects are sent; it
will not emit one in the middle of another JSON object response.

#### Request format

Sending the `since` command is simply a matter of formatting it as JSON.  Note
that the JSON text must be a single line (don't send a pretty printed version
of it!) and be followed by a newline `\n` character:

    ["since", "/path/to/src", "n:c_srcs", "*.c"]

## Future

 * persist the registered command triggers across process invocations
 * add a command to remove registered triggers
 * add a richer output mode for the `since` command that passes on
   file status information in addition to the filename.
 * add a hybrid of since and trigger that allows a connected client
   to receive notifications of file changes over their unix socket
   connection as they happen.  Disconnecting the client will disable
   those particular notifications.

## License

Watchman is made available under the terms of the Apache License 2.0.  See the
LICENSE file that accompanies this distribution for the full text of the
license.

