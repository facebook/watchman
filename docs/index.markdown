---
title: Watchman | A file watching service
layout: index
permalink: index.html
---

# Watchman

A file watching service.

## Purpose

Watchman exists to watch files and record when they actually change.  It can
also trigger actions (such as rebuilding assets) when matching files change.

## System Requirements

Watchman is known to compile and pass its test suite on:

 * Linux systems with `inotify`
 * OS X (uses FSEvents)
 * BSDish systems (FreeBSD 9.1, OpenBSD 5.2) that have the
   `kqueue(2)` facility
 * Illumos and Solaris style systems that have `port_create(3C)`

Watchman relies on the operating system facilities for file notification,
which means that you will likely have very poor results using it on any
kind of remote or distributed filesystem.

Watchman does not currently support Windows or any other operating system
not covered by the list above.

## Build/Install

You can use these steps to get watchman built:

```bash
./autogen.sh
./configure
make
```
[![Build Status](https://travis-ci.org/facebook/watchman.png)](https://travis-ci.org/facebook/watchman)

## Concepts

 * Watchman can watch one or more directory trees.
 * Each watched tree is referred to as a "root"
 * A root is watched recursively
 * Watchman does not follow symlinks.  It knows they exist, but they show up
   the same as any other file in its reporting.
 * Watchman waits for a watched root to settle down before it will start
   to trigger notifications or command execution.
 * Watchman is conservative, preferring to err on the side of caution, which
   means that it considers the files to be freshly changed when you start to
   watch them.

## Usage

Watchman is provided as a single executable binary that acts as both the
watchman service and a client of that service.  The simplest usage is entirely
from the command line, but real world scenarios will typically be more complex
and harness the streaming JSON protocol directly.

## Quickstart

There are a number of options and a good bit of reference material in this
document.  Here's a quick overview for getting going to help you figure out
which parts of the doc you want to read.

These two lines establish a watch on a source directory and then set up a
trigger named "buildme" that will run a tool named "minify-css" whenever a CSS
file is changed.  The tool will be passed a list of the changed filenames.

```bash
$ watchman watch ~/src
# the single quotes around '*.css' are important, don't leave them out!
$ watchman -- trigger ~/src buildme '*.css' -- minify-css
```
