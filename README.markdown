# Watchman

A file watching service.

## Purpose

Watchman exists to watch files and record when they actually change.  It can
also trigger actions (such as rebuilding assets) when matching files change.

## System Requirements

Watchman is known to compile and pass its test suite on:

 * Linux systems with `inotify`
 * OS X (uses FSEvents)
 * BSDish systems (FreeBSD 9.1, OpenBSD 5.2) that have the
   `kqueue(2)` facility
 * Illumos and Solaris style systems that have `port_create(3C)`

Watchman relies on the operating system facilities for file notification,
which means that you will likely have very poor results using it on any
kind of remote or distributed filesystem.

Watchman does not currently support Windows or any other operating system
not covered by the list above.

## Build/Install

You can use these steps to get watchman built:

```bash
./autogen.sh
./configure
make
```
[![Build Status](https://travis-ci.org/facebook/watchman.png)](https://travis-ci.org/facebook/watchman)

## Concepts

 * Watchman can watch one or more directory trees.
 * Each watched tree is referred to as a "root"
 * A root is watched recursively
 * Watchman does not follow symlinks.  It knows they exist, but they show up
   the same as any other file in its reporting.
 * Watchman waits for a watched root to settle down before it will start
   to trigger notifications or command execution.
 * Watchman is conservative, preferring to err on the side of caution, which
   means that it considers the files to be freshly changed when you start to
   watch them.

## Usage

Watchman is provided as a single executable binary that acts as both the
watchman service and a client of that service.  The simplest usage is entirely
from the command line, but real world scenarios will typically be more complex
and harness the streaming JSON protocol directly.

## Quickstart

There are a number of options and a good bit of reference material in this
document.  Here's a quick overview for getting going to help you figure out
which parts of the doc you want to read.

These two lines establish a watch on a source directory and then set up a
trigger named "buildme" that will run a tool named "minify-css" whenever a CSS
file is changed.  The tool will be passed a list of the changed filenames.

```bash
$ watchman watch ~/src
# the single quotes around '*.css' are important, don't leave them out!
$ watchman -- trigger ~/src buildme '*.css' -- minify-css
```

## Querying information about a watched root

Watchman maintains a view of the filesystem tree for each watched root.  It
provides two flavors of query syntax for matching file information; one is
structured and expressive and easy for tools to compose queries, but cumbersome
to use for quick ad-hoc queries that you might type on the command line.

We call the former "File Queries" and the latter "Legacy Patterns".

File queries are expressed as a JSON object that specifies how the tree is
processed, which files are of interest and which fields should be returned.
They are described in more detail in the **File Queries** section below.

Legacy patterns follow a more traditional UNIX command line approach of
using command line switches to indicate the nature of the pattern match.
When legacy patterns are used, the result set unconditionally includes
all core file metadata fields.  They are described in more detail in the
**Legacy Pattern Syntax** section below.

## Legacy Pattern syntax

Where you see `[patterns]` in the command syntax for the `find`, `since` and
`trigger` commands, we allow filename patterns that match according the
following rules:

 * We maintain an *inclusion* and an *exclusion* list.  As the arguments
   are processed we'll accumulate them in one or the other.  By default
   they are accumulated into the *inclusion* list.
 * `-X` causes any subsequent items to be placed into the *exclusion* list
 * `-I` causes any subsequent items to be placed into the *inclusion* list
 * `--` indicates the end of the set of patterns
 * `-p` indicates that the following pattern should use `pcre` as the
   expression term.  This is reset after generating the next term.
 * `-P` indicates that the following pattern should use `ipcre` as the
   expression term and perform a case insensitive match.  This is reset
   after generating the next term.
 * If neither `-p` nor `-P` were used, the generated term will use `match`
 * `!` followed by a space followed by a pattern will negate the sense of
   the pattern match generating a `not` term.

Any elements in the inclusion list will match; they are composed together
using an "anyof" term.

The inclusion list and exclusion lists are composed using the logic `(NOT
anyof exclusion) AND (anyof inclusion)`.

For example:

     *.c

Generates a file expression:

```json
["match", "*.c", "wholename"]
```

A list:

    *.js *.css

```json
["anyof",
  ["match", "*.js", "wholename"],
  ["match", "*.css", "wholename"]
]
```

An example of how the exclusion list syntax works:

     -X *.c -I *main*

Generates:

```json
["allof",
  ["not", ["match", "*.c", "wholename"]],
  ["match", "*main*", "wholename"]
]
```

## File Queries

Watchman file queries consist of 1 or more *generators* that feed files through
the expression evaluator.

### Generators

Watchman provides 4 generators:

 * **since**: generates a list of files that were modified since a specific
   clockspec. If this is not specified, this will be treated the same as if a
   clockspec from a different instance of watchman was passed in. You can
   use either a string oclock value, or a integer number of epoch seconds.
 * **suffix**: generates a list of files that have a particular suffix or set
   of suffixes.  The value can be either a string or an array of strings.
 * **path**: generates a list of files based on their path and depth. Depth
   controls how far watchman will search down the directory tree for files.
   Depth = 0 means only files and directories which are contained in this
   path.  The value of path can be either an array, a string, or an object.
   If it is a string, it is treated as a path, and depth is infinite.  If
   an object, the fields path (a string) and depth (an integer) must be
   supplied.  An array can contain either strings or objects, each with the
   same meaning as single strings or objects.  Paths are relative to
   the root, so if watchman is watching /foo/, path "bar" refers to /foo/bar.
 * **all**: generates a list of all known files

Generators are analogous to the list of *paths* that you specify when using the
`find(1)` utility, but are implemented in watchman with a bit of a twist
because watchman doesn't need to crawl the filesystem in realtime and instead
maintains a couple of indexes over the tree.

A query may specify any number of generators; each generator will emit its list
of files and this may mean that you see the same file output more than once if
you specified the use of multiple generators that all produce the same file.

### Expressions

A watchman query expression consists of 0 or more expression terms.  If no
terms are provided then each file evaluated is considered a match (equivalent
to specifying a single `true` expression term).

Otherwise, the expression is evaluated against the file and produces a boolean
result.  If that result is true then the file is considered a match and is
added to the output set.

An expression term is canonically represented as a JSON array whose zeroth
element is a string containing the term name.

    ["termname", arg1, arg2]

If the term accepts no arguments you may use a short form that consists of just
the term name expressed as a string:

    "true"

Expressions that match against file names may match against either the
*basename* or the *wholename* of the file.  The basename is the name of the
file within its containing directory.  The wholename is the name of the file
relative to the watched root.

#### allof

The `allof` expression term evaluates as true if all of the grouped expressions
also evaluated as true.  For example, this expression matches only files whose
name ends with `.txt` and that are not empty files:

    ["allof", ["match", "*.txt"], ["not", "empty"]]

Each array element after the term name is evaluated as an expression of its own:

    ["allof", expr1, expr2, ... exprN]

Evaluation of the subexpressions stops at the first one that returns false.

#### anyof

The `anyof` expression term evaluates as true if any of the grouped expressions
also evaluated as true.  The following expression matches files whose name ends
with either `.txt` or `.md`:

    ["anyof", ["match", "*.txt"], ["match", "*.md"]]

Each array element after the term name is evaluated as an expression of its own:

    ["anyof", expr1, expr2, ... exprN]

Evaluation of the subexpressions stops at the first one that returns true.

#### not

The `not` expression inverts the result of the subexpression argument:

    ["not", "empty"]

#### true

The `true` expression always evaluates as true.

    "true"
    ["true"]

#### false

The `false` expression always evaluates as false.

    "false"
    ["false"]

#### suffix

The `suffix` expression evaluates true if the file suffix matches the second
argument.  This matches files name `foo.php` and `foo.PHP` but not `foophp`:

    ["suffix", "php"]

Suffix expression matches are case insensitive.

#### match and imatch

The `match` expression performs an `fnmatch(3)` match against the basename of
the file, evaluating true if the match is successful.

    ["match", "*.txt"]

You may optionally provide a third argument to change the scope of the match
from the basename to the wholename of the file.

    ["match", "*.txt", "basename"]
    ["match", "dir/*.txt", "wholename"]

`match` is case sensitive; for case insensitive matching use `imatch` instead;
it behaves identically to `match` except that the match is performed ignoring
case.

#### pcre and ipcre

The `pcre` expression performs a Perl Compatible Regular Expression match
against the basename of the file.  This pattern matches `test_plan.php` but not
`mytest_plan`:

    ["pcre", "^test_"]

You may optionally provide a third argument to change the scope of the match
from the basename to the wholename of the file.

    ["pcre", "txt", "basename"]
    ["pcre", "txt", "wholename"]

`pcre` is case sensitive; for case insensitive matching use `ipcre` instead;
it behaves identically to `pcre` except that the match is performed ignoring
case.

To use this feature, you must configure watchman `--with-pcre`.

#### name and iname

The `name` expression performs exact matches against file names.  By default it
is scoped to the basename of the file:

    ["name", "Makefile"]

You may specify multiple names to match against by setting the second argument
to an array:

    ["name", ["foo.txt", "Makefile"]]

This second form can be accelerated and is preferred over an `anyof`
construction.

You may change the scope of the match via the optional third argument:

    ["name", "path/to/file.txt", "wholename"]
    ["name", ["path/to/one", "path/to/two"], "wholename"]

Finally, you may specify case insensitive evaluation by using `iname` instead
of `name`.

##### type

Evaluates as true if the type of the file matches that specified by the second
argument; this matches regular files:

    ["type", "f"]

Possible types are:

 * **b**: block special file
 * **c**: character special file
 * **d**: directory
 * **f**: regular file
 * **p**: named pipe (fifo)
 * **l**: symbolic link
 * **s**: socket
 * **D**: Solaris Door

##### empty

Evaluates as true if the file exists, has size 0 and is a regular file or
directory.

    "empty"
    ["empty"]

##### exists

Evaluates as true if the file exists

    "exists"
    ["exists"]

##### since

Evaluates as true if the specified time property of the file is greater than
the since value.  Note that this is not the same as the `since` generator; when
used as an expression term we are performing a straight clockspec comparison.
When used as a generator, candidate files are selected based on the `since`
time index.  The end result might or might not be the same --- in particular, if
the `since` time index is not passed in, it will be treated the same as a fresh
instance, and only files that exist will be returned. The efficiency can vary
based on the size and shape of the file tree that you are watching; it may be
cheaper to generate the candidate set of files by suffix and then check the
modification time if many files were changed since your last query.

This will yield a true value if the observed change time is more recent than
the specified clockspec (this is equivalent to specifying "oclock" as the third
parameter):

     ["since", "c:12345:234"]

You may specify particular fields from the filesystem metadata.  In this case
your clockspec should be a unix time value:

     ["since", 12345668, "mtime"]
     ["since", 12345668, "ctime"]

You may explicitly request the observed clock values too; in these cases we'll
accept either a timestamp or a clock value.  The `oclock` is the last observed
change clock value (observed clock) and the `cclock` is the clock value where
we first observed the file come into existence (created clock):

     ["since", 12345668, "oclock"]
     ["since", "c:1234:123", "oclock"]
     ["since", 12345668, "cclock"]
     ["since", "c:1234:2342", "cclock"]



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

### Command: watch-list

Returns a list of watched dirs.

From the command line:

```bash
watchman watch-list
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

### Command: watch-del

Removes a watch.

From the command line:

```bash
watchman watch-del /path/to/dir
```

JSON:

```json
["watch-del", "/path/to/dir"]
```

Unless `no-state-save` is in use, the removed watch will also be removed
from the state file and will not be automatically watched if/when watchman
is restarted.

### Command: clock

Returns the current clock value for a watched root.

Be careful how you interpret this value; it returns the instantaneous value of
the clock, and may have changed by the time you intend to act upon it.

```bash
watchman clock /path/to/dir
```

JSON:

```json
["clock", "/path/to/dir"]
```

### Command: trigger

The trigger command will create or replace a trigger.

A trigger is a saved incremental query over a watched root.  When files
change that match the query expression, Watchman will spawn a process and
pass information about the changed files to it.

Triggered processes are run by the Watchman server process that runs in
the background; they do not have access to your terminal and their
output is redirected (by default) to the Watchman log file.

Watchman waits for the filesystem to settle before processing any
triggers, batching the list of changed files together before invoking
the registered command.  You can adjust the settle period via the
`.watchmanconfig` file.

Note that deleted files are counted as changed files and are passed the command
in exactly the same way as changed-but-existing files.

Watchman will only run a single instance of the trigger process at a time.
That avoids fork-bomb style behavior in cases where your trigger also modifies
files.  When the process terminates, watchman will re-evaluate the trigger
criteria based on the clock at the time the process was last spawned; if
a file list is generated watchman will spawn a new child with the files
that changed in the meantime.

Unless `no-save-state` is in use, triggers are saved and re-established
across a Watchman process restart.  If you had triggeres saved prior to
upgrading to Watchman 2.9.7, those triggers will be forgotten as you upgrade
past version 2.9.7; you will need to re-register them.

There are two syntaxes for registering triggers; a legacy syntax that allows
very simple trigger configuration with some reasonable defaults.  The second is
the extended trigger syntax which is available since Watchman version 2.9.7.
The legacy syntax is implemented in terms of the extended syntax and is
preserved for backwards compatibility with older clients.

#### Extended syntax

Starting in Watchman 2.9.7, you may use the extended JSON trigger definition
syntax detailed below.  It provides more control over how the triggered
commands are invoked than was possible in earlier versions.

JSON:

```json
["trigger", "/path/to/dir", <triggerobj>]
```

Where `triggerobj` is a trigger configuration object with the fields
defined below.

Here's an example trigger specified via the CLI that will cause `make` to
be run whenever assets or sources are changed:

```bash
watchman -j <<-EOT
["trigger", "/path/to/root", {
  "name": "assets",
  "expression": ["pcre", "\.(js|css|c|cpp)$"],
  "command": ["make"]
}]
EOT
```

The possible trigger object properties are:

* `name` defines the name of the trigger.  You may use this name to
  remove the trigger later.  Registering a different trigger with the same name
  as an existing trigger will implicitly delete the old trigger and then
  register the new one, causing the trigger expression to be evaluated for the
  whole tree.

* `command` specifies the command to invoke.  It must be an array of string
  values; this will form the argv array of the trigger process.  When
  the trigger is spawned, the `$PATH` of the Watchman process will be used
  to locate the command.  If you have changed your `$PATH` since the Watchman
  process was started, Watchman won't be able to see your new `$PATH`.
  If you are registering trigger that runs something from an unusual or
  non-default location, it is recommended that you specify the full path
  to that command.  If you are registering a trigger script that can
  be found in the watched root, just specify the path relative to the
  root.

* `append_files` is an optional boolean parameter; if enabled, the `command`
  array will have the set of matching file names appended when the trigger
  is invoked.  System limits such as `sysconf(_SC_ARG_MAX)`
  and/or `RLIMIT_STACK` set an upper bound on the size of the parameters
  and environment that are passed to a spawned process.  Watchman will
  try to ensure that the command is runnable by keeping the number of
  file name arguments below the system limits.  If the full set cannot be
  passed to the process, Watchman will pass as many as it thinks will fit and
  omit the rest.  When this argument list truncation occurs, Watchman will
  export `WATCHMAN_FILES_OVERFLOW=true` into the environment so that the child
  process can determine that this has happened.  Watchman cannot break
  the arguments apart and run multiple processes for each argument batch;
  for that functionality, use `xargs(1)` for the `command` and set the
  `stdin` property to `NAME_PER_LINE`.

* `expression` accepts a query expression.  The expression is applied
  to the list of changed files to generate the set of files that are
  relevant to this trigger.  If no files match, the command will not be
  invoked.  Omitting the expression will match all changed files.

* `stdin` specifies how stdin should be configured for the command
  invocation.  You may set the value of this property to one of the following:

    * the string value `/dev/null` - sets stdin to read from `/dev/null`.
      This is the default and will be used if you omit the `stdin` property.

    * an array value will be interpreted as a list of field names.  When
      the command is invoked, Watchman will generate an array of JSON objects
      that contain those field names on stdin.  For example, if `stdin` is set
      to `["name", "size"]`, stdin will be a JSON array containing the list
      of changed files, represented as objects with the `name` and `size`
      properties: `[{"name": "filename.txt", "size": 123}]`.
      The list of valid fields is the same as the same as that
      documented in the `query` command.  Just as with the `query` command,
      if the field list is comprised of a single field then the JSON
      will be an array of those field values.  For instance, if you set
      `stdin` to `["name"]` the JSON will be of the form `["filename.txt"]`
      instead of `[{"name": "filename.txt"}]`.

    * the string value `NAME_PER_LINE` will cause Watchman to generate a list
      of file names on stdin, one name per line.  No quoting will be applied to
      the names, and they may contain spaces.

* `stdout` and `stderr` control the output and error streams.  If omitted,
  the corresponding stream will be inherited from the Watchman process, which
  typically means that the command output/error stream will show up in the
  Watchman log file.  If specified, the value must be a string:

    * `>path/to/file` - causes output to redirected to the specifie file.
      The path is relative to the watched root, and will be truncated
      prior to being written to, if it exists, or created if it does not
      exist.

    * `>>path/to/file` - causes output to redirected to the specifie file.
      The path is relative to the watched root.  If the file already exists
      then it will be appended to.  The file will be created if it does not
      exist.

* `max_files_stdin` specifies a limit on the number of files reported on
  stdin when stdin is set to hold the set of matched files.  If the number of
  files that matched exceeds this limit, the input will be truncated to match
  this limit and `WATCHMAN_FILES_OVERFLOW=true` will also be exported into the
  environment.  The default, if omitted, is no limit.

* `chdir` can be used to specify the working directory that should be set
  prior to spawning the process.  The default is to set the working directory
  to the watched root.  The value of this property is a string that will be
  interpreted relative to the watched root.  Note that changing the working dir
  does not cause the file names from the query result to be re-written: they
  will *always* be relative to the watched root.  The path to the root can
  be found in the `$WATCHMAN_ROOT` environmental variable.

#### Legacy syntax

The legacy syntax is easier to execute from the CLI than the JSON based
extended syntax, but doesn't allow all of the trigger options to be set.
In addition, it only supports the **Legacy Pattern Syntax** for queries.

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

Note the single quotes around the `*.js`; if you omit them, your shell
will expand it to a list of file names and register those in the trigger.
While this would work, any `*.js` files that you add after registering the
trigger will not cause the trigger to run.

or in JSON:

```json
["trigger", "/home/wez/www", "jsfiles", "*.js", "--", "ls", "-l"]
```

The legacy syntax is interpreted as a trigger object with the following
settings:

* `name` is set to the `triggername`
* `command` is set to the `<cmd>` list
* `expression` is generated from the `<patterns>` list using the rules laid
  out in **Legacy Pattern Syntax** elsewhere in this document.
* `append_files` is set to `true`
* `stdin` is set to `["name", "exists", "new", "size", "mode"]`
* `stdout` and `stderr` will be set to output to the Watchman log file
* `max_files_stdin` will be left unset

For this legacy example, if `~/www/scripts/foo.js` is changed,
watchman will chdir to `~/www` then invoke `ls -l scripts/foo.js`.  Note that
the output will show up in the Watchman log file, not in your terminal.

#### Environment for trigger commands

Since Watchman version 2.9.7, the following environment variables are set
for all trigger commands, even those registered using the simple trigger
syntax:

* `WATCHMAN_FILES_OVERFLOW` is set to `true` if the number of files exceeds
  either the `max_files_stdin` limit or the system argument size limit.
* `WATCHMAN_CLOCK` is set to the current clock at the time of the trigger
  invocation
* `WATCHMAN_SINCE` is set to the clock value of the prior trigger invocation,
  or unset if this is the first trigger invocation.
* `WATCHMAN_ROOT` is set to the path to the watched root
* `WATCHMAN_TRIGGER` is set to the name of the trigger
* `WATCHMAN_SOCK` is set to the path to the Watchman socket, so that you
  can figure out how to connect back to Watchman.

### Command: trigger-list

Returns the set of registered triggers associated with a root directory.

```bash
watchman trigger-list /root
```

Note that the format of the output from `trigger-list` changed in Watchman
version 2.9.7.  It will now output a list of trigger objects as defined
by the `trigger` command.

### Command: trigger-del

Deletes a named trigger from the list of registered triggers.  This disables
and removes the trigger from both the in-memory and the saved state lists.

```bash
watchman trigger-del /root triggername
```

### Command: find

Finds all files that match the optional list of patterns under the
specified dir.  If no patterns were specified, all files are returned.

```bash
watchman find /path/to/dir [patterns]
```

### Command: query

Available starting in version 1.6

```bash
watchman -j <<-EOT
["query", "/path/to/root", {
  "suffix": "php",
  "expression": ["allof",
    ["type", "f"],
    ["not", "empty"],
    ["ipcre", "test", "basename"]
  ],
  "fields": ["name"]
}]
EOT
```

Executes a query against the specified root. This example uses the `-j` flag to
the watchman binary that tells it to read stdin and interpret it as the JSON
request object to send to the watchman service.  This flag allows you to send
in a pretty JSON object (as shown above), but if you're using the socket
interface you must still format the object as a single line JSON request as
documented in the protocol spec.

The first argument to query is the path to the watched root.  The second
argument holds a JSON object describing the query to be run.  The query object
is processed by passing it to the query engine (see **File Queries** above)
which will generate a set of matching files.

The query command will then consult the `fields` member of the query object;
if it is not present it will default to:

```json
"fields": ["name", "exists", "new", "size", "mode"]
```

For each file in the result set, the query command will generate a JSON object
value populated with the requested fields.  For example, the default set of
fields will return a response something like this:

```json
{
    "version": "2.9",
    "clock": "c:80616:59",
    "is_fresh_instance": false,
    "files": [
        {
            "exists": true,
            "mode": 33188,
            "new": false,
            "name": "argv.c",
            "size": 1340,
        }
    ]
}
```

For queries using the `since` generator, the `is_fresh_instance` member is true
if the particular clock value indicates that it was returned by a different
instance of watchman, or a named cursor hasn't been seen before. In that case,
only files that currently exist will be returned, and all files will have `new`
set to `true`. For all other queries, is_fresh_instance will always be true.
Advanced users may set the input parameter `empty_on_fresh_instance` to true,
in which case no files will be returned for fresh instances.

If the `fields` member consists of a single entry, the files result will be a
simple array of values; ```"fields": ["name"]``` produces:

```json
{
    "version": "1.5",
    "clock": "c:80616:59",
    "files": ["argv.c", "foo.c"]
}
```

#### Available fields

 * `name` - string: the filename, relative to the watched root
 * `exists` - bool: true if the file exists, false if it has been deleted
 * `cclock` - string: the "created clock"; the clock value when we first
            observed the file, or the clock value when it last switched
            from !exists to exists.
 * `oclock` - string: the "observed clock"; the clock value where we last
            observed some change in this file or its metadata.
 * `ctime`, `ctime_ms`, `ctime_us`, `ctime_ns`, `ctime_f`
            - last inode change time measured in integer seconds, milliseconds,
              microseconds, nanoseconds or floating point seconds
              respectively.
 * `mtime`, `mtime_ms`, `mtime_us`, `mtime_ns`, `mtime_f`
            - modified time measured in integer seconds, milliseconds,
              microseconds, nanoseconds or floating point seconds
              respectively.
 * `size` - integer: file size in bytes
 * `mode` - integer: file (or directory) mode expressed as a decimal integer
 * `uid` - integer: the owning uid
 * `gid` - integer: the owning gid
 * `ino` - integer: the inode number
 * `dev` - integer: the device number
 * `nlink` - integer: number of hard links
 * `new` - bool: whether this entry is newer than the `since` generator
           criteria

#### Synchronization timeout (since 2.1)

By default a `query` will wait for up to 2 seconds for the view of the
filesystem to become current.  Watchman decides that the view is current by
creating a cookie file and waiting to observe the notification that it is
present.  If the cookie is not observed within the sync_timeout period then the
query invocation will error out with a synchronization error message.

If your synchronization requirements differ from the default, you may pass in
your desired timeout when you construct your query; it must be an integer value
expressed in milliseconds:

```json
["query", "/path/to/root", {
  "expression": ["exists"],
  "fields": ["name"],
  "sync_timeout": 2000
}]
```

You may specify `0` as the value if you do not wish for the query to create
a cookie and synchronize; the query will be evaluated over the present view
of the tree, which may lag behind the present state of the filesystem.

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
    "version": "2.7",
    "is_fresh_instance": true,
    "clock": "c:80616:59",
    "files": [
        {
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
watchman --server-encoding=json --persistent log-level debug
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

### Command: subscribe

Available starting in version 1.6

Subscribes to changes against a specified root and requests that they be sent
to the client via its connection.  The updates will continue to be sent while
the connection is open.  If the connection is closed, the subscription is
implicitly removed.

This makes the most sense in an application connecting via the socket
interface, but you may also subscribe via the command line tool if you're
interested in observing the changes for yourself:

```bash
watchman -j --server-encoding=json -p <<-EOT
["subscribe", "/path/to/root", "mysubscriptionname", {
  "expression": ["allof",
    ["type", "f"],
    ["not", "empty"],
    ["suffix", "php"]
  ],
  "fields": ["name"]
}]
EOT
```

The example above registers a subscription against the specified root with the
name `mysubscriptionname`.

The response to a subscribe command looks like this:

```json
{
  "version":   "1.6",
  "subscribe": "mysubscriptionname"
}
```

When the subscription is first established, the
expression term is evaluated and if any files match, a subscription notification
packet is generated and sent, unilaterally to the client.

Then, each time a change is observed, and after the settle period has passed,
the expression is evaluated again.  If any files are matched, the server will
unilaterally send the query results to the client with a packet that looks like
this:

```json
{
  "version": "1.6",
  "clock": "c:1234:123",
  "files": ["one.php"],
  "root":  "/path/being/watched",
  "subscription": "mysubscriptionname"
}
```

The subscribe command object allows the client to specify a since parameter; if
present in the command, the initial set of subscription results will only
include files that changed since the specified clockspec, equivalent to using
the `query` command with the `since` generator.

```json
["subscribe", "/path/to/root", "myname", {
  "since": "c:1234:123",
  "expression": ["not", "empty"],
  "fields": ["name"]
}]
```

The suggested mode of operation is for the client process to maintain its own
local copy of the last "clock" value and use that to establish the subscription
when it first connects.

### Command: unsubscribe

Available starting in version 1.6

Cancels a named subscription against the specified root.  The server side
will no longer generate subscription packets for the specified subscription.

```json
["unsubscribe", "/path/to/root", "mysubscriptionname"]
```

### Command: get-sockname

If you're integrating against watchman using the unix socket and either the
JSON or BSER protocol, you may need to discover the correct socket path.
Rather than hard-coding the path or replicating the logic discussed in the
**Environment and Files** section, you can simply execute the CLI to determine
the path.  This has the side effect of spawning the service for your user if it
was not already running--bonus!

```bash
$ watchman get-sockname
{
  "version": "2.5",
  "sockname": "/tmp/.watchman.wez"
}
```

## Clockspec

For commands that query based on time, watchman offers a couple of different
ways to measure time.

 * number of seconds since the unix epoch (unix `time_t` style)
 * **recommended for most users**: a named cursor of the form `n:whatever`
 * **recommended for advanced users**: a clock id of the form `c:123:234`

The first and most obvious is passing a unix timestamp.  Watchman records
the observed time that files change and allows you to find files that have
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
clock spec. However, it's not possible to "roll back" a named cursor, so
advanced users desiring such functionality should use clock ids instead.

## Command Line Options

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

## Configuration Files

When watching a root, if a valid JSON file named `.watchmanconfig` is present
in the root, watchman will load and associate the file with the root.

Watchman will try to resolve certain configuration parameters using the
following logic:

 * If there is a .watchmanconfig and the option is present there, use that
   value.
 * If the option was specified on the command line, use that value
 * Look at the global configuration file. By default, this is at the location
   configured using `--enable-conffile` (default `/etc/watchman.json`), but this
   can be overridden by setting the environment variable `WATCHMAN_CONFIG_FILE`.
   If this file is a valid JSON file, and contains the option, use that value
 * Otherwise use an appropriate default for that option.

### Configuration Parameters

The following parameters are accepted in the .watchmanconfig and global
configuration files:

 * `settle` - specifies the settle period in *milliseconds*.  This controls
   how long the filesystem should be idle before dispatching triggers.
   The default value is 20 milliseconds.

The following parameters are accepted in the global configuration file only:

 * `root_restrict_files` - specifies a list of files, at least one of which
   should be present in a directory for watchman to add it as a root. By default
   there are no restrictions.

   For example,

   ```json
   {
     "root_restrict_files": [".git", ".hg"]
   }
   ```

   will allow watches only in the top level of Git or Mercurial repositories.

The following parameters are accepted in the .watchmanconfig file only,
since version 2.9.3:

 * `ignore_vcs` - apply special VCS ignore logic to the set of named dirs.
   This option has a default value of `[".git", ".hg", ".svn"]`.  Dirs that
   match this option are observed and watched using special shallow logic.
   The shallow watch allows watchman to mildly abuse the version control
   directories to store its query cookie files and to observe VCS locking
   activity without having to watch the entire set of VCS data for large trees.

 * `ignore_dirs` - dirs that match are completely ignored by watchman.
   This is useful to ignore a directory that contains only build products and
   where file change notifications are unwanted because of the sheer volume of
   files.

   For example,

   ```json
   {
     "ignore_dirs": ["build"]
   }
   ```

   would ignore the `build` directory at the top level of the watched tree,
   and everything below it.  It will never appear in the watchman query
   results for the tree.

The following parameters are accepted in the .watchmanconfig file since
version 2.9.4:

 * `gc_age_seconds` - Deleted files (and dirs) older than this are
   periodically pruned from the internal view of the filesystem.  Until
   they are pruned, they will be visible to queries but will have their
   `exists` field set to `false`.   Once they are pruned, watchman will
   remember the most recent clock value of the pruned nodes.  Any since
   queries based on a clock prior to the last prune clock will be treated
   as a fresh instance query.  This allows a client to detect and choose
   how to handle the case where they have missed changes.
   See `is_fresh_instance` elsewhere in this document for more information.
   The default for this is `43200` (12 hours).

 * `gc_interval_seconds` - how often to check for, and prune out, deleted
   nodes per the `gc_age_seconds` option description above.
   The default for this is `86400` (24 hours).  Set this to `0` to disable
   the periodic pruning operation.

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
extremely low (256!).  More recent versions of watchman (2.9.2 and later)
use FSEvents and are not so sensitive to descriptor limits.

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
Feel free to use the GitHub issue tracker and pull requests discuss and
submit code changes.

We (Facebook) have to ask for a "Contributor License Agreement" from someone
who sends in a patch or code that we want to include in the codebase.  This is
a legal requirement; a similar situation applies to Apache and other ASF
projects.

If we ask you to fill out a CLA we'll direct you to [our online CLA
page](https://developers.facebook.com/opensource/cla) where you can complete it
easily.  We use the same form as the Apache CLA so that friction is minimal.

### Tools

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

    arc unit --everything

If you'd like to contribute a patch to watchman, we'll ask you to make sure
that `arc unit` still passes successfully and we'd ideally like you to augment
the test suite to cover the functionality that you're adding or changing.


## Known Issues

 * Watchman does not follow symlinks. This is by design and is very unlikely to
   change.
 * Watchman has no notion of case-insensitivity, as seen on HFS+ on OS X. This
   document on [case-folding](docs/casefolding.markdown) explains
   if you should care about it.

## License

Watchman is made available under the terms of the Apache License 2.0.  See the
LICENSE file that accompanies this distribution for the full text of the
license.
