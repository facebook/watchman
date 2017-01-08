---
pageid: compatibility
title: Compatibility Rules
layout: docs
category: Compatibility
permalink: docs/compatibility.html
---

`watchman` has been used in production since a few weeks after it was first
written, and thus it has always made an effort to be backward compatible across
releases and platforms.

* Commands and options will never be removed, but new ones may be added.
* We may *deprecate* commands and options and remove them from documentation,
  but they will still continue to work forever.
* Whenever a command or option is deprecated, we will provide a suitable
  alternative.
* Bugfixes might cause minor behavior changes -- these changes will usually be
documented in release notes.

`watchman` does not follow [semantic versioning](http://semver.org)!

* Since its public APIs never make incompatible changes, MAJOR versions are
  moot.
* While in the past we've released versions with three components (x.y.z),
  starting version 3.1 the version number will only have two components that
  are meaningful (x.y), with the third component always zero.
* The version after 3.9 is expected to be 4.0.  The version number string
  reported by these versions will be 3.9.0 and 4.0.0 respectively.

*Since 3.8.*

`watchman` introduces [capabilities](capabilities.html) to describe new
or optional features.  You can use the [expanded version command](
/watchman/docs/cmd/version.html) to query capabilities and avoid building
knowledge of version numbers in your client application(s).
