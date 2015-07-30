---
id: expr.match
title: match & imatch
layout: docs
category: Expression Terms
permalink: docs/expr/match.html
---

The `match` expression performs a glob-style match against the basename of
the file, evaluating true if the match is successful.

    ["match", "*.txt"]

You may optionally provide a third argument to change the scope of the match
from the basename to the wholename of the file.

    ["match", "*.txt", "basename"]
    ["match", "dir/*.txt", "wholename"]

If you want to recursively match all files under a directory, use the `**`
glob operator along with the `wholename` scope:

    ["match", "src/**/*.java", "wholename"]

By default, paths whose names start with `.` are not included. To
change this behavior, you may optionally provide a fourth argument
containing a dictionary of flags:

    ["match", "*.txt", "basename", {"includedotfiles": true}]

By default, backslashes in the pattern escape the next character, so
`\*` matches a literal `*` character. To change this behavior so
backslashes are treated literally, set the `noescape` flag to `true`
in the flags dictionary. (Note that `\\` is a literal `\` in JSON notation):

    ["match", "*\\*.txt", "filename", {"noescape": true}]

matches `a\b.txt`.

`match` is case sensitive; for case insensitive matching use `imatch` instead;
it behaves identically to `match` except that the match is performed ignoring
case.

Starting in version 2.9.9, on OS X systems where the watched root is a case
insensitive filesystem (this is the common case for OS X), `match` is equivalent
to `imatch`.
