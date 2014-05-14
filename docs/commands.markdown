---
id: commands
title: about watchman commands
layout: docs
category: Commands
permalink: docs/commands.html
next: casefolding
---

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
