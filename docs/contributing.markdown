---
id: contributing
title: Contributing
layout: docs
permalink: contributing.html
category: Internals
---

If you're thinking of hacking on watchman we'd love to hear from you!
Feel free to use the GitHub issue tracker and pull requests discuss and
submit code changes.

We (Facebook) have to ask for a "Contributor License Agreement" from someone
who sends in a patch or code that we want to include in the codebase.  This is
a legal requirement; a similar situation applies to Apache and other ASF
projects.

If we ask you to fill out a CLA we'll direct you to [our online CLA
page](https://code.facebook.com/cla) where you can complete it
easily.  We use the same form as the Apache CLA so that friction is minimal.

### Getting Started

You need to be able to build watchman from source and run its test suite.
You will need:

* python
* automake
* autoconf
* libpcre

```
$ git clone https://github.com/facebook/watchman.git
$ cd watchman
$ ./autogen.sh
$ ./configure
$ make
```

After making a change, run the integration tests to make sure that things
are still working well before you submit your pull request:

```
$ make integration
```

We'll probably ask you to augment the test suite to cover the functionality
that you're adding or changing.

