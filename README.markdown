# Watchman

A file watching service.

## Purpose

Watchman exists to watch files and record when they actually change.  It can
also trigger actions (such as rebuilding assets) when matching files change.

## System Requirements

Watchman is known to compile and pass its test suite on:

 * Linux systems with `inotify`
 * OS X and BSDish systems (FreeBSD 9.1, OpenBSD 5.2) that have the
   `kqueue(2)` facility
 * Illumos and Solaris style systems that have `port_create(3C)`

Watchman relies on the operating system facilities for file notification,
which means that you will likely have very poor results using it on any
kind of remote or distributed filesystem.

## Concepts

 * Watchman does not follow symlinks.  It knows they exist, but they show up
   the same as any other file in its reporting.
 * Watchman encourages you to avoid race conditions by providing an abstract
   clock notation, named cursors to aid in maintaining
   proper state around polling operations and a command trigger facility.
 * Watchman waits for a watched directory to settle down before it will start
   to trigger notifications or command execution.
 * Watchman is conservative, preferring to err on the side of caution, which
   means that it considers the files to be freshly changed when you start to
   watch them.

## Usage

Watchman is provided as a single executable binary that acts as both the
watchman service and a client of that service.  The simplest usage is entirely
from the command line, but real world scenarios will typically be more complex
and harness the streaming JSON protocol directly.

## Command Line Options

The following options are recognized if they are present on the command line
before any non-option arguments.

```bash
 -U, --sockname=PATH   Specify alternate sockname

 -o, --logfile=PATH    Specify path to logfile

 -p, --persistent      Persist and wait for further responses

 -n, --no-save-state   Don't save state between invocations

 --statefile=PATH      Specify path to file to hold watch and trigger state

 -f, --foreground      Run the service in the foreground

 -s, --settle          Number of milliseconds to wait for filesystem to settle
```

 * See `log-level` for an example of when you might use the `persistent` flag
 * The default `settle` period is 20 milliseconds

## Environment and Files

Watchman will use the `$USER` or `$LOGNAME` environmental variables to determine
the current userid, and the `$TMPDIR` or `$TMP` environmental variables to
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

## Available Commands

A summary of the watchman commands follows.  The commands can be executed
either by the command line tool or via the JSON protocol.  When using the
command line, be aware of shell quoting; you should make a point of quoting
any filename patterns that you want watchman to process, otherwise the shell
will expand the pattern and pass a literal list of the files that matched
at the time you ran the command, which may be very different from the set
of commands that match at the time a change is detected!

A quick note on JSON: in this documentation we show JSON in a human readable
pretty-printed form.  The watchman client executable itself will pretty-print
its output too.  The actual JSON protocol uses newlines to separate JSON
packets.  If you're implementing a JSON client, make sure you read the section
on the JSON protocol carefully to make sure you get it right!

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

### Command: watch

Requests that the specified dir is watched for changes.
Watchman will track all files and dirs rooted at the specified path.

From the command line:

```bash
watchman watch ~/www
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

### Command: trigger

Sets up a trigger such that if any files under the specified dir that match the
specified set of patterns change, then the specified command will be run and
passed the list of matching files.

From the command line:

```bash
watchman -- trigger /path/to/dir triggername [patterns] -- [cmd]
```

Note that the first `--` is to distinguish watchman CLI switches from the
second `--`, which delimits patterns from the trigger command.  This is only
needed when using the CLI, not when using the JSON protocol.

JSON:
```json
["trigger", "/path/to/dir", "triggername", <patterns>, "--", <cmd>]
```

For example:

```bash
watchman -- trigger ~/www jsfiles '*.js' -- ls -l
```

or in JSON:

```json
["trigger", "/home/wez/www", "jsfiles", "*.js", "--", "ls", "-l"]
```

If, say, `~/www/scripts/foo.js` is changed, then watchman will chdir
to `~/www` then invoke `ls -l scripts/foo.js`.

Watchman waits for the filesystem to settle before processing any
triggers, batching the list of changed files together before invoking
the registered command.

Deleted files are counted as changed files and are passed the command in
exactly the same way.

Watchman will arrange for the standard input stream of the triggered
process to read from a file that contains a JSON array of changed file
information.  This is in the same format as the file list returned by
the `since` command.

Watchman will limit the length of the command line so that it does not
exceed the system configured argument length limit, which you can see
for yourself by running `getconf ARG_MAX`.

If the length limit would be exceeded, watchman simply stops appending
arguments.  Any that are not appended are discarded; **watchman will not
queue up and execute a second instance of the trigger process**.  It
cannot do this in a race-free manner.  If you are watching a large
directory and there is a risk that you'll exceed the command line
length limit, then you should use the JSON input stream instead, as
this has no size limitation.

Watchman will only run a single instance of the trigger process at a time.
That avoids fork-bomb style behavior in cases where your trigger also modifies
files.  When the process terminates, watchman will re-evaluate the trigger
criteria based on the clock at the time the process was last spawned; if
a file list is generated watchman will spawn a new child with the files
that changed in the meantime.

Unless `no-save-state` is in use, triggers are saved and re-established
across a process restart.

### Command: trigger-list

Returns the set of registered triggers associated with a root directory.

```bash
watchman trigger-list /root
```

### Command: find

Finds all files that match the optional list of patterns under the
specified dir.  If no patterns were specified, all files are returned.

```bash
watchman find /path/to/dir [patterns]
```

### Command: since

```bash
watchman since /path/to/dir <clockspec> [patterns]
```

Finds all files that were modified since the specified clockspec that
match the optional list of patterns.  If no patterns are specified,
all modified files are returned.

The response includes a `files` array, each element of which is an
object with fields containing information about the file:

```json
{
    "version": "1.1",
    "clock": "c:80616:59",
    "files": [
        {
            "atime": 1357797739,
            "cclock": "c:80616:1",
            "ctime": 1357617635,
            "dev": 16777220,
            "exists": true,
            "gid": 100,
            "ino": 20161390,
            "mode": 33188,
            "mtime": 1357617635,
            "name": "argv.c",
            "nlink": 1,
            "oclock": "c:80616:39",
            "size": 1340,
            "uid": 100
        }
    ]
}
```

The fields should be largely self-explanatory; they correspond to
fields from the underlying `struct stat`, but a couple need special
mention:

 * **cclock** - The "created" clock; the clock value representing the time that
this file was first observed, or the clock value where this file changed from
deleted to non-deleted state.
 * **oclock** - The "observed" clock; the clock value representing the time
that this file was last observed to have changed.
 * **exists** - whether we believe that the file exists on disk or not.  If
this is false, most of the other fields will be omitted.
 * **new** - this is only set in cases where the file results were generated as
part of a time or clock based query, such as the `since` command.  If the
`cclock` value for the file is newer than the time you specified then the file
entry is marked as `new`.  This allows you to more easily determine if the file
was newly created without having to maintain a lot of state.

### Command: log-level

Changes the log level of your connection to the watchman service.

From the command line:

```bash
watchman --persistent log-level debug
```

JSON:

```json
["log-level", "debug"]
```

This command changes the log level of your client session.  Whenever watchman
writes to its log, it walks the list of client sessions and also sends a log
packet to any that have their log level set to match the current log event.

Valid log levels are:

 * `debug` - receive all log events
 * `error` - receive only important log events
 * `off`   - receive no log events

Note that you cannot tap into the output of triggered processes using this
mechanism.

Log events are sent unilaterally by the server as they happen, and have
the following structure:

```json
{
  "version": "1.0",
  "log": "log this please"
}
```

### Command: log

Generates a log line in the watchman log.

```bash
watchman log debug "log this please"
```

### Command: shutdown-server

This command causes your watchman service to exit with a normal status code.

## Clockspec

For commands that query based on time, watchman offers a couple of different
ways to measure time.

 * number of seconds since the unix epoch (unix `time_t` style)
 * clock id of the form `c:123:234`
 * **recommended**: a named cursor of the form `n:whatever`

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

```bash
watchman since /path/to/src n:c_srcs *.c
```

and when you run it a second time, it will show you only the "C" source files
that changed since the last time that someone queried using "n:c_srcs" as the
clock spec.

## Build/Install

You can use these steps to get watchman built:

```bash
./autogen.sh
./configure
make
```

## System Specific Preparation

### Linux inotify Limits

The `inotify(7)` subsystem has three important tunings that impact watchman.

 * `/proc/sys/fs/inotify/max_user_instances` impacts how many different
   root dirs you can watch.
 * `/proc/sys/fs/inotify/max_user_watches` impacts how many dirs you
   can watch across all watched roots.
 * `/proc/sys/fs/inotify/max_queued_events` impacts how likely it is that
   your system will experience a notification overflow.

You obviously need to ensure that `max_user_instances` and `max_user_watches`
are set so that the system is capable of keeping track of your files.

`max_queued_events` is important to size correctly; if it is too small, the
kernel will drop events and watchman won't be able to report on them.  Making
this value bigger reduces the risk of this happening.

Watchman has two simple strategies for mitigating an overflow of
`max_queued_events`:

 * It uses a dedicated thread to consume kernel events as quickly as possible
 * When the kernel reports an overflow, watchman will assume that all the files
   have been modified and will re-crawl the directory tree as though it had just
   started watching the dir.

This means that if an overflow does occur, you won't miss a legitimate change
notification, but instead will get spurious notifications for files that
haven't actually changed.

### Max OS File Descriptor Limits

The default per-process descriptor limit on current versions of OS X is
extremely low (256!).  Since kqueue() requires an open descriptor for each
watched file, you will very quickly run into resource limits if you do not
raise them in your system configuration.

Watchman will attempt to raise its descriptor limit to match
`kern.maxfilesperproc` when it starts up, so you shouldn't need to mess
with `ulimit`; just raising the sysctl should do the trick.

The following will raise the limits to allow 10 million files total, with 1
million files per process until your next reboot.

    sudo sysctl -w kern.maxfiles=10485760
    sudo sysctl -w kern.maxfilesperproc=1048576

Putting the following into a file named `/etc/sysctl.conf` on OS X will cause
these values to persist across reboots:

    kern.maxfiles=10485760
    kern.maxfilesperproc=1048576


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

 * Each request is a JSON array followed by a newline.
 * Each response is a JSON object followed by a newline.

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

    ["since", "/path/to/src", "n:c_srcs", "*.c"] <NEWLINE>

## Contributing?

If you're thinking of hacking on watchman we'd love to hear from you!
Feel free to use the Github issue tracker and pull requests discuss and
submit code changes.

We use a tool called `arc` to run tests and perform lint checks.  `arc` is part
of [Phabricator](http://www.phabricator.org) and can be installed by following
these steps:

```bash
mkdir /somewhere
cd /somewhere
git clone git://github.com/facebook/libphutil.git
git clone git://github.com/facebook/arcanist.git
```

Add `arc` to your path:

```bash
export PATH="$PATH:/somewhere/arcanist/bin/"
```

With `arc` in your path, re-running configure will detect it and adjust the
makefile so that `arc lint` will be run as part of `make`, but you can run it
yourself outside of make.

You can run the unit tests using:

    arc unit

If you'd like to contribute a patch to watchman, we'll ask you to make sure
that `arc unit` still passess successfully and we'd ideally like you to augment
the test suite to cover the functionality that you're adding or changing.


## Future

 * add a command to remove registered triggers
 * add a hybrid of since and trigger that allows a connected client
   to receive notifications of file changes over their unix socket
   connection as they happen.  Disconnecting the client will disable
   those particular notifications.
 * Watchman does not currently follow symlinks.  It would be nice if it
   did, but doing so will add some complexity.

## License

Watchman is made available under the terms of the Apache License 2.0.  See the
LICENSE file that accompanies this distribution for the full text of the
license.

