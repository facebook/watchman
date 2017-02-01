---
pageid: contributing
title: Contributing
layout: docs
permalink: contributing.html
section: Internals
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
* libtool (or glibtool on OS X)
* libpcre
* libfolly (only needed if building the cppclient library)
* nodejs (for fb-watchman)

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

Please keep in mind that our versioning philosophy in Watchman is to provide
an *append only* API.  If you're changing functionality, we'll ask you to do
so in such a way that it won't break older clients of Watchman.

### Don't forget the docs

If you're changing or adding new functionality, we'll ask you to also update
the documentation.   You will need `ruby` 2.0.0 or later to preview the
documentation.

One time setup:

```bash
$ cd website
$ sudo gem install bundler
$ sudo bundler install
```

Then:

```bash
$ jekyll serve -w -t
```

This will print out a URL that you can open in your browser to preview your
documentation changes.

The source for the documentation is in the `website/_docs` dir in markdown
format.
