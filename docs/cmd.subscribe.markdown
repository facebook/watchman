---
id: cmd.subscribe
title: subscribe
layout: docs
category: Commands
permalink: docs/cmd/subscribe.html
---

Available starting in version 1.6

Subscribes to changes against a specified root and requests that they be sent
to the client via its connection.  The updates will continue to be sent while
the connection is open.  If the connection is closed, the subscription is
implicitly removed.

This makes the most sense in an application connecting via the socket
interface, but you may also subscribe via the command line tool if you're
interested in observing the changes for yourself:

```bash
$ watchman -j --server-encoding=json -p <<-EOT
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
