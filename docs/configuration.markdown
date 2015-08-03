---
id: config
title: Configuration Files
layout: docs
category: Invocation
permalink: docs/config.html
---

Watchman looks for configuration files in two places:

 * The global configuration file `/etc/watchman.json`
 * The root specific configuration file `.watchmanconfig`

When watching a root, if a valid JSON file named `.watchmanconfig` is present
in the root directory, watchman will load it and use it as a source of
configuration information specific to that root.

The global configuration path can be changed by passing the
`--enable-conffile` option to configure when you build watchman.  This
documentation refers to it as `/etc/watchman.json` throughout, just be aware
that your particular installation may locate it elsewhere.   In addition,
the environmental variable `$WATCHMAN_CONFIG_FILE` will override the
default location.

Changes to the `.watchmanconfig` or `/etc/watchman.json` files are not picked
up automatically; you will need to remove and re-add the watch (for
`.watchmanconfig`) or restart watchman (for `/etc/watchman.json`) for those
changes to take effect.

### Resolution / Scoping

There are three configuration scopes:

 * **local** - the option value is read from the `.watchmanconfig` file in the
   associated root.
 * **global** - the option value is read from the `/etc/watchman.json` file
 * **fallback** - the option value is read from the `.watchmanconfig` file.
   If the option was not present in the `.watchmanconfig` file, then read
   it from the `/etc/watchman.json` file.

This table shows the scoping and availability of the various options:

Option | Scope | Since version
-------|-------|--------------
`settle` | local |
`root_restrict_files` | global | deprecated in 3.1
`root_files` | global | 3.1
`enforce_root_files` | global | 3.1
`illegal_fstypes` | global | 2.9.8
`illegal_fstypes_advice` | global | 2.9.8
`ignore_vcs` | local | 2.9.3
`ignore_dirs` | local | 2.9.3
`gc_age_seconds` | local | 2.9.4
`gc_interval_seconds` | local | 2.9.4
`fsevents_latency` | fallback | 3.2
`idle_reap_age_seconds` local | 3.7

### Configuration Options

#### settle

Specifies the settle period in *milliseconds*.  This controls how long the
filesystem should be idle before dispatching triggers.  The default value is 20
milliseconds.

#### root_files

Available starting in version 3.1

Specifies a list of files that, if present in a directory, identify that
directory as the root of a project.

If left unspecified, to aid in transitioning between versions, watchman will
use the value of the now deprecated
[root_restrict_files](#root_restrict_files) configuration setting.

If neither `root_files` nor `root_restrict_files` is specified in the
configuration, watchman will use a default value consisting of:

* `.git`
* `.hg`
* `.svn`
* `.watchmanconfig`

Watchman will add `.watchmanconfig` to whatever value is specified for
this configuration value if it is not present.

This example causes only `.watchmanconfig` to be considered as a project
root file:

```json
{
  "root_files": [".watchmanconfig"]
}
```

See the [watch-project](cmd/watch-project.html) command for more information.

#### enforce_root_files

Available starting in version 3.1

This is a boolean option that defaults to `false`.  If it is set to `true`
then the [watch](cmd/watch.html) command will only succeed if the requested
directory contains one of the files listed by the [root_files](#root_files)
configuration option, and the [watch-project](cmd/watch-project.html) command
will only succeed if a valid project root is found.

If left unspecified, to aid in transitioning between versions, watchman will
check to see if the now deprecated [root_restrict_files](#root_restrict_files)
configuration setting is present.  If it is found then the effective value of
`enforce_root_files` is set to `true`.

#### root_restrict_files

Deprecated starting in version 3.1; use [root_files](#root_files) and
[enforce_root_files](#enforce_root_files) to effect the same behavior.

Specifies a list of files, at least one of which should be present in a
directory for watchman to add it as a root. By default there are no
restrictions.

For example,

```json
{
  "root_restrict_files": [".git", ".hg"]
}
```

will allow watches only in the top level of Git or Mercurial repositories.

#### illegal_fstypes

Specifies a list of filesystem types that watchman is prohibited to attempt to
watch.  Watchman will determine the filesystem type of the root of a watch; if
the typename is present in the `illegal_fstypes` list, the watch will be
prohibited.  You may also specify `illegal_fstypes_advice` as a string with
additional advice to your user.  The purpose of this configuration option is
largely to prevent the use of Watchman on network mounted filesystems.  On
Linux systems, Watchman may not be able to determine the precise type name of a
mounted filesystem.  If the filesystem type is not known to watchman, it will
be reported as `unknown`.

For example,

```json
{
  "illegal_fstypes": ["nfs", "cifs", "smb"],
  "illegal_fstypes_advice": "use a local directory"
}
```

will prevent watching dirs mounted on network filesystems and provide the
advice to use a local directory.  You may omit the `illegal_fstypes_advice`
setting to use a default suggestion to relocate the directory to local disk.

#### ignore_vcs

Apply special VCS ignore logic to the set of named dirs.  This option has a
default value of `[".git", ".hg", ".svn"]`.  Dirs that match this option are
observed and watched using special shallow logic.  The shallow watch allows
watchman to mildly abuse the version control directories to store its query
cookie files and to observe VCS locking activity without having to watch the
entire set of VCS data for large trees.

#### ignore_dirs

Dirs that match are completely ignored by watchman.  This is useful to ignore a
directory that contains only build products and where file change notifications
are unwanted because of the sheer volume of files.

For example,

```json
{
  "ignore_dirs": ["build"]
}
```

would ignore the `build` directory at the top level of the watched tree, and
everything below it.  It will never appear in the watchman query results for
the tree.

Since version 2.9.9, if you list a dir in `ignore_dirs` that is also listed in
`ignore_vcs`, the `ignore_dirs` placement will take precedence.  This may not
sound like a big deal, but since `ignore_vcs` is used as a hint to for the
placement of [cookie files](/watchman/docs/cookies.html), having these two
options overlap in earlier versions would break watchman queries.

#### gc_age_seconds

Deleted files (and dirs) older than this are periodically pruned from the
internal view of the filesystem.  Until they are pruned, they will be visible
to queries but will have their `exists` field set to `false`.   Once they are
pruned, watchman will remember the most recent clock value of the pruned nodes.
Any since queries based on a clock prior to the last prune clock will be
treated as a fresh instance query.  This allows a client to detect and choose
how to handle the case where they have missed changes.  See `is_fresh_instance`
elsewhere in this document for more information.  The default for this is
`43200` (12 hours).

#### gc_interval_seconds

How often to check for, and prune out, deleted nodes per the `gc_age_seconds`
option description above.  The default for this is `86400` (24 hours).  Set
this to `0` to disable the periodic pruning operation.

#### fsevents_latency

Controls the latency parameter that is passed to `FSEventStreamCreate` on OS X.
The value is measured in seconds.  The fixed value of this parameter prior to
version 3.2 of watchman was `0.0001` seconds.  Starting in version 3.2 of
watchman, the default is now `0.01` seconds and can be controlled on a
per-root basis.

If you observe problems with `kFSEventStreamEventFlagUserDropped` increasing
the latency parameter will allow the system to batch more change notifications
together and operate more efficiently.

#### idle_reap_age_seconds

*Since 3.7*

How many seconds a watch can remain idle before becoming a candidate for
reaping, measured in seconds.  The default for this is `432000` (5 days).  Set
this to `0` to prevent reaping.

A watch is considered to be idle when it has had no commands that operate on it
for `idle_reap_age_seconds`.   If an idle watch has no triggers and no
subscriptions then it will be cancelled, releasing the associated operating
system resources, and removed from the state file.
