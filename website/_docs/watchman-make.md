---
id: watchman-make
title: watchman-make
layout: docs
category: Invocation
permalink: docs/watchman-make.html
---

`watchman-make` is a convenience tool to help automatically invoke your build
tool in response to files changing.  It is useful to automate building assets
or running tests as you save files during development.

`watchman-make` will establish a watch on the files you specify and remain
running in the foreground, waiting for changes to occur.  When a change is
triggered, your build tool will be run in the foreground with its output being
passed through to your terminal session (or wherever you may have redirected
it).

Events are consolidated and settled before they are dispatched to your build
tool so that it won't start executing until after the files have stopped
changing.  The `--settle` argument controls the settle duration.

`watchman-make` requires `pywatchman` (and thus requires `python`) as well as
`watchman`.

### Example

```bash
$ watchman-make -p '*.c' '*.h' 'Makefile*' -t all -p 'tests/**/*.py' 'tests/**/*.c' -t integration
# Relative to /Users/wez/fb/watchman
# Changes to files matching *.c *.h Makefile* will execute `make all`
# Changes to files matching tests/**/*.py tests/**/*.c will execute `make integration`
# waiting for changes
```

### Targets

`watchman-make` is target-centric; you tell it about one or more build targets
and their dependencies, and it will then trigger the build for those targets as
changes are detected.

The example above defines two targets using the `-t` argument; `all` and
`integration`.  These correspond to targets with the same names in the watchman
`Makefile`.  Each target will pick up the list of patterns defined by the `-p`
argument that precedes it.

```bash
$ watchman-make -p '*.c' '*.h' -t all
```

The above defines a target named `all` that will be triggered whenever any
combination of files are changed that have filenames that match either of the
patterns `*.c` or `*.h`.   When it triggers, `watchman-make` will execute
`make all`.

If you don't use `make`, you can use the `--make` option to tell `watchman-make`
to use your builder of choice.  When a target is triggered, `watchman-make` will
concatenate the value of `--make` with the name of the target and execute that
command using the shell.

The target name has no special meaning to `watchman-make`, it is used only to
construct the command to invoke.  There is no special logic or support that is
specific to Makefiles or make.

### Multiple Targets

There are two different ways to specify multiple targets.  The first is shown
in the main example at the top of this page and is repeated here:

```bash
$ watchman-make -p '*.c' '*.h' 'Makefile*' -t all -p 'tests/**/*.py' 'tests/**/*.c' -t integration
```

This defines two independent targets, `all` and `integration` that each have a
list of patterns defined as their triggers.  Each time you specify a target
using the `-t` option, the value of the `-p` option is cleared.

The above will cause `make all` to be run if you change a file that matches
`.*c`, and will cause `make integration` to run if you change a source file
under the tests directory.

An alternative is to list multiple target names with your `-t` option:

```bash
$ watchman-make -p '*.c' '*.h' 'Makefile*' 'tests/**/*.py' 'tests/**/*.c' -t all integration
```

this will execute `make all integration` if you change any `*.c` file *or* test
source file.

