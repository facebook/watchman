---
id: file-query
title: File Queries
layout: docs
category: Queries
permalink: docs/file-query.html
next: simple-query
---

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

### Relative roots

*Since 3.3.*

Watchman supports optionally evaluating queries with respect to a path within a
watched root. This is used with the `relative_root` parameter:

```json
["query", "/path/to/watched/root", {
  "relative_root": "project1",
}]
```

Setting a relative root results in the following modifications to queries:

* The `path` generator is evaluated with respect to the relative root. In the
  above example, `"path": ["dir"]` will return all files inside
  `/path/to/watched/root/project1/dir`.
* The input expression is evaluated with respect to the relative root. In the
  above example, `"expression": ["match", "dir/*.txt", "wholename"]` will return
  all files inside `/path/to/watched/root/project1/dir/` that match the glob
  `*.txt`.
* Paths inside the relative root are returned with the relative root stripped
  off. For example, a path `project1/dir/file.txt` would be returned as
  `dir/file.txt`.
* Paths outside the relative root are not returned.

Relative roots behave similarly to a separate Watchman watch on the
subdirectory, without any of the system overhead that that imposes. This is
useful for large repositories, where your script or tool is only interested in a
particular directory inside the repository.
