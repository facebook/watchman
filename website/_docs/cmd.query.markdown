---
id: cmd.query
title: query
layout: docs
section: Commands
permalink: docs/cmd/query.html
---

*Since 1.6.*

```bash
$ watchman -j <<-EOT
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
is processed by passing it to the query engine (see [File Queries](
/watchman/docs/file-query.html)) which will generate a set of matching files.

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

### Available fields

 * `name` - string: the filename, relative to the watched root
 * `exists` - bool: true if the file exists, false if it has been deleted
 * `cclock` - string: the "created clock"; the clock value when we first
observed the file, or the clock value when it last switched from
!exists to exists.
 * `oclock` - string: the "observed clock"; the clock value where we last
observed some change in this file or its metadata.
 * `ctime`, `ctime_ms`, `ctime_us`, `ctime_ns`, `ctime_f` -
last inode change time measured in integer seconds, milliseconds,
microseconds, nanoseconds or floating point seconds respectively.
 * `mtime`, `mtime_ms`, `mtime_us`, `mtime_ns`, `mtime_f` -
modified time measured in integer seconds, milliseconds,
microseconds, nanoseconds or floating point seconds respectively.
 * `size` - integer: file size in bytes
 * `mode` - integer: file (or directory) mode expressed as a decimal integer
 * `uid` - integer: the owning uid
 * `gid` - integer: the owning gid
 * `ino` - integer: the inode number
 * `dev` - integer: the device number
 * `nlink` - integer: number of hard links
 * `new` - bool: whether this entry is newer than the `since` generator criteria
 * `type` - string: the file type. (Available since version 3.1).  Has the
   the values listed in [the type query expression](../expr/type.html)

### Synchronization timeout (since 2.1)

By default a `query` will wait for up to 60 seconds for the view of the
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
  "sync_timeout": 60000
}]
```

You may specify `0` as the value if you do not wish for the query to create
a cookie and synchronize; the query will be evaluated over the present view
of the tree, which may lag behind the present state of the filesystem.
