---
id: expr.pcre
title: pcre & ipcre
layout: docs
category: Expression Terms
permalink: docs/expr/pcre.html
---

*To use this feature, you must configure watchman `--with-pcre`!*

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

Starting in version 2.9.9, on OS X systems where the watched root is a case
insensitive filesystem (this is the common case for OS X), `pcre` is equivalent
to `ipcre`.
