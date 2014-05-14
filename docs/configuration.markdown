---
id: config
title: Configuration Files
layout: docs
category: Invocation
permalink: docs/config.html
---

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

 * `illegal_fstypes` (since version 2.9.8) - specifies a list of filesystem
   types that watchman is prohibited to attempt to watch.  Watchman will
   determine the filesystem type of the root of a watch; if the typename is
   present in the `illegal_fstypes` list, the watch will be prohibited.  You
   may also specify `illegal_fstypes_advice` as a string with additional advice
   to your user.  The purpose of this configuration option is largely to
   prevent the use of Watchman on network mounted filesystems.  On Linux
   systems, Watchman may not be able to determine the precise type name of a
   mounted filesystem.  If the filesystem type is not known to watchman, it
   will be reported as `unknown`.

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
