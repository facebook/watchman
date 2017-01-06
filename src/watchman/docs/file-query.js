/**
 * @generated
 * @jsx React.DOM
 */
var React = require("React");
var layout = require("DocsLayout");
module.exports = React.createClass({
  render: function() {
    return layout({metadata: {"id":"file-query","title":"File Queries","layout":"docs","category":"Queries","permalink":"docs/file-query.html","next":"simple-query"}}, `
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
\`find(1)\` utility, but are implemented in watchman with a bit of a twist
because watchman doesn't need to crawl the filesystem in realtime and instead
maintains a couple of indexes over the tree.

A query may specify any number of generators; each generator will emit its list
of files and this may mean that you see the same file output more than once if
you specified the use of multiple generators that all produce the same file.

### Expressions

A watchman query expression consists of 0 or more expression terms.  If no
terms are provided then each file evaluated is considered a match (equivalent
to specifying a single \`true\` expression term).

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

The \`allof\` expression term evaluates as true if all of the grouped expressions
also evaluated as true.  For example, this expression matches only files whose
name ends with \`.txt\` and that are not empty files:

    ["allof", ["match", "*.txt"], ["not", "empty"]]

Each array element after the term name is evaluated as an expression of its own:

    ["allof", expr1, expr2, ... exprN]

Evaluation of the subexpressions stops at the first one that returns false.

#### anyof

The \`anyof\` expression term evaluates as true if any of the grouped expressions
also evaluated as true.  The following expression matches files whose name ends
with either \`.txt\` or \`.md\`:

    ["anyof", ["match", "*.txt"], ["match", "*.md"]]

Each array element after the term name is evaluated as an expression of its own:

    ["anyof", expr1, expr2, ... exprN]

Evaluation of the subexpressions stops at the first one that returns true.

#### not

The \`not\` expression inverts the result of the subexpression argument:

    ["not", "empty"]

#### true

The \`true\` expression always evaluates as true.

    "true"
    ["true"]

#### false

The \`false\` expression always evaluates as false.

    "false"
    ["false"]

#### suffix

The \`suffix\` expression evaluates true if the file suffix matches the second
argument.  This matches files name \`foo.php\` and \`foo.PHP\` but not \`foophp\`:

    ["suffix", "php"]

Suffix expression matches are case insensitive.

#### match and imatch

The \`match\` expression performs an \`fnmatch(3)\` match against the basename of
the file, evaluating true if the match is successful.

    ["match", "*.txt"]

You may optionally provide a third argument to change the scope of the match
from the basename to the wholename of the file.

    ["match", "*.txt", "basename"]
    ["match", "dir/*.txt", "wholename"]

\`match\` is case sensitive; for case insensitive matching use \`imatch\` instead;
it behaves identically to \`match\` except that the match is performed ignoring
case.

Starting in version 2.9.9, on OS X systems where the watched root is a case
insensitive filesystem (this is the common case for OS X), \`match\` is equivalent
to \`imatch\`.

#### pcre and ipcre

The \`pcre\` expression performs a Perl Compatible Regular Expression match
against the basename of the file.  This pattern matches \`test_plan.php\` but not
\`mytest_plan\`:

    ["pcre", "^test_"]

You may optionally provide a third argument to change the scope of the match
from the basename to the wholename of the file.

    ["pcre", "txt", "basename"]
    ["pcre", "txt", "wholename"]

\`pcre\` is case sensitive; for case insensitive matching use \`ipcre\` instead;
it behaves identically to \`pcre\` except that the match is performed ignoring
case.

To use this feature, you must configure watchman \`--with-pcre\`.

Starting in version 2.9.9, on OS X systems where the watched root is a case
insensitive filesystem (this is the common case for OS X), \`pcre\` is equivalent
to \`ipcre\`.

#### name and iname

The \`name\` expression performs exact matches against file names.  By default it
is scoped to the basename of the file:

    ["name", "Makefile"]

You may specify multiple names to match against by setting the second argument
to an array:

    ["name", ["foo.txt", "Makefile"]]

This second form can be accelerated and is preferred over an \`anyof\`
construction.

You may change the scope of the match via the optional third argument:

    ["name", "path/to/file.txt", "wholename"]
    ["name", ["path/to/one", "path/to/two"], "wholename"]

Finally, you may specify case insensitive evaluation by using \`iname\` instead
of \`name\`.

Starting in version 2.9.9, on OS X systems where the watched root is a case
insensitive filesystem (this is the common case for OS X), \`name\` is equivalent
to \`iname\`.

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
the since value.  Note that this is not the same as the \`since\` generator; when
used as an expression term we are performing a straight clockspec comparison.
When used as a generator, candidate files are selected based on the \`since\`
time index.  The end result might or might not be the same --- in particular, if
the \`since\` time index is not passed in, it will be treated the same as a fresh
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
accept either a timestamp or a clock value.  The \`oclock\` is the last observed
change clock value (observed clock) and the \`cclock\` is the clock value where
we first observed the file come into existence (created clock):

     ["since", 12345668, "oclock"]
     ["since", "c:1234:123", "oclock"]
     ["since", 12345668, "cclock"]
     ["since", "c:1234:2342", "cclock"]

##### size

Available since version 3.1

The size term allows the size of an existing file (not deleted) to be evaluated
using simple relational operators as described in the table below.

The size term must always be an array with 3 elements:

     ["size", "gt", 0]

The second parameter describes the relational operator and the third parameter
is the integer *operand* to compare against.   The example above evaluates to
\`true\` if the file exists and its size is greater than zero.

Possible relational operators are:

Operator | Meaning | Result
---------|---------|-------
\`eq\`     | Equal                 | \`true\` if file exists and \`size == operand\`
\`ne\`     | Not Equal             | \`true\` if file exists and \`size != operand\`
\`gt\`     | Greater Than          | \`true\` if file exists and \`size > operand\`
\`ge\`     | Greater Than Or Equal | \`true\` if file exists and \`size >= operand\`
\`lt\`     | Less Than             | \`true\` if file exists and \`size < operand\`
\`le\`     | Less Than Or Equal    | \`true\` if file exists and \`size <= operand\`

#### dirname and idirname

Available since version 3.1

The \`dirname\` term allows matching on the parent directory structure for a
given file.

For the examples below, given a file with a wholename (the relative path from
the project root) of \`foo/bar/baz\`, the dirname portion is \`foo/bar\`.

The following two terms will match any file whose dirname is either exactly a
match for \`foo/bar\` or is any child directory of \`foo/bar\`.  The first of these
two is a shortcut for the second:

      ["dirname", "foo/bar"]
      ["dirname", "foo/bar", ["depth", "ge", 0]]

The second of those terms uses a relational expression based on the depth of
the file within the specified dirname.  A file is considered to have
\`depth == 0\` if it is contained directly within the specified dirname.  It has
\`depth == 1\` if it is contained in a direct child directory of the specified
dirname, \`depth == 2\` if it is contained in a grand-child directory and so on.

The relational expression accepts the same relational operators as described in
the [size term](#size) above.

If you wanted to match only files that were directly in the \`foo/bar\` dir:

      ["dirname", "foo/bar", ["depth", "eq", 0]]

If you wanted to match only files that were in a grand-child or deeper:

      ["dirname", "foo/bar", ["depth", "ge", 2]]

\`idirname\` is the case insensitive version of \`dirname\`.  If the watched root
is detected as a case insensitive fileystem, \`dirname\` is equivalent to \`idirname\`.

### Relative roots

*Since 3.3.*

Watchman supports optionally evaluating queries with respect to a path within a
watched root. This is used with the \`relative_root\` parameter:

\`\`\`json
["query", "/path/to/watched/root", {
  "relative_root": "project1",
}]
\`\`\`

Setting a relative root results in the following modifications to queries:

* The \`path\` generator is evaluated with respect to the relative root. In the
  above example, \`"path": ["dir"]\` will return all files inside
  \`/path/to/watched/root/project1/dir\`.
* The input expression is evaluated with respect to the relative root. In the
  above example, \`"expression": ["match", "dir/*.txt", "wholename"]\` will return
  all files inside \`/path/to/watched/root/project1/dir/\` that match the glob
  \`*.txt\`.
* Paths inside the relative root are returned with the relative root stripped
  off. For example, a path \`project1/dir/file.txt\` would be returned as
  \`dir/file.txt\`.
* Paths outside the relative root are not returned.

Relative roots behave similarly to a separate Watchman watch on the
subdirectory, without any of the system overhead that that imposes. This is
useful for large repositories, where your script or tool is only interested in a
particular directory inside the repository.
`);
  }
});
