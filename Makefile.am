bin_PROGRAMS = watchman
# ensure that we have a reasonable default for the python install
DESTDIR ?= /

doc_DATA = README.markdown
docdir = ${prefix}/share/doc/watchman-$(VERSION)

THIRDPARTY_CPPFLAGS = -I$(top_srcdir)/thirdparty/jansson -I$(top_builddir)/thirdparty/jansson
JSON_LIB = -L. libwmanjson.a

watchman_CPPFLAGS = $(THIRDPARTY_CPPFLAGS) @IRONMANCFLAGS@
watchman_LDADD = $(JSON_LIB) $(ART_LIB) libwildmatch.a
watchman_SOURCES = \
	argv.c       \
	envp.c       \
	spawn.c       \
	opt.c        \
	cfg.c        \
	clockspec.c  \
	checksock.c  \
	fstype.c     \
	log.c        \
	ignore.c     \
	json.c       \
	bser.c       \
	expflags.c   \
	hash.c       \
	ht.c         \
	ioprio.c        \
	opendir.c       \
	pending.c       \
	perf.c          \
	stream.c        \
	stream_stdout.c \
	stream_unix.c   \
	timedlock.c     \
	cmds/find.c     \
	cmds/info.c     \
	cmds/log.c      \
	cmds/query.c    \
	cmds/since.c    \
	cmds/reg.c      \
	cmds/state.c    \
	cmds/subscribe.c    \
	cmds/trigger.c  \
	cmds/watch.c    \
	cmds/debug.c    \
	query/base.c       \
	query/dirname.c    \
	query/parse.c      \
	query/eval.c       \
	query/intcompare.c \
	query/type.c       \
	query/suffix.c     \
	query/match.c      \
	query/pcre.c       \
	query/name.c       \
	query/fieldlist.c  \
	query/since.c      \
	query/empty.c      \
	watcher/auto.c     \
	watcher/fsevents.c \
	watcher/helper.c   \
	watcher/inotify.c  \
	watcher/kqueue.c   \
	watcher/portfs.c   \
	launchd.c    \
	listener.c   \
	listener-user.c   \
	clientmode.c \
	main.c       \
	root.c       \
	state.c      \
	string.c     \
	time.c

noinst_HEADERS =   \
	watchman.h       \
	watchman_hash.h

noinst_LIBRARIES = \
	libart.a \
	libcritbit.a \
	libtap.a \
	libwildmatch.a \
	libwmanjson.a

# bundled json library
libwmanjson_a_CPPFLAGS = $(THIRDPARTY_CPPFLAGS)
libwmanjson_a_SOURCES = \
	thirdparty/jansson/dump.c \
	thirdparty/jansson/error.c \
	thirdparty/jansson/hashtable.c \
	thirdparty/jansson/hashtable.h \
	thirdparty/jansson/jansson_private.h \
	thirdparty/jansson/load.c \
	thirdparty/jansson/memory.c \
	thirdparty/jansson/pack_unpack.c \
	thirdparty/jansson/strbuffer.c \
	thirdparty/jansson/strbuffer.h \
	thirdparty/jansson/strconv.c \
	thirdparty/jansson/utf.c \
	thirdparty/jansson/utf.h \
	thirdparty/jansson/value.c

# bundled testing library
libtap_a_CPPFLAGS = $(THIRDPARTY_CPPFLAGS)
libtap_a_SOURCES = thirdparty/tap.c
TAP_LIB = libtap.a

# pattern matching library
libwildmatch_a_SOURCES = thirdparty/wildmatch/wildmatch.c
WILDMATCH_LIB = libwildmatch.a

# adaptive radix tree library
libart_a_SOURCES = thirdparty/libart/src/art.c
ART_LIB = libart.a

# critbit tree library
libcritbit_a_SOURCES = thirdparty/critbit.c
CRITBIT_LIB = libcritbit.a

# unit tests
TESTS = \
		tests/art.t \
		tests/argv.t \
		tests/bser.t \
		tests/ignore.t \
		tests/critbit.t \
		tests/log.t \
		tests/wildmatch.t
noinst_PROGRAMS = $(TESTS)

if HAVE_ARC
# Run lint and output stuff suitable for feeding into ":make" in vim
# quickfix mode.
lint:
	-arc lint --output compiler --lintall
else
lint:
endif

export ARCHFLAGS=@ARCHFLAGS@
if HAVE_PYTHON
py-build:
	cd python && $(PYTHON) ./setup.py clean build_py -c -d . build_ext -i

py-install:
	cd python && $(PYTHON) ./setup.py install --prefix ${prefix} --root $(DESTDIR)

# This is invoked via WatchmanIntegrationEngine
py-tests:
	$(PYTHON) $(TESTNAME)

py-integration:
	$(PYTHON) runtests.py --keep-if-fail

py-clean:
	-cd python && $(PYTHON) ./setup.py clean --all

else
py-build:
py-tests:
py-clean:
py-install:
py-integration:
	@echo You need python to run the tests
	false
endif

if HAVE_RUBY
rb-build:
	cd ruby/ruby-watchman && bundle && bundle exec rake make

rb-tests:
	cd ruby/ruby-watchman && bundle && bundle exec rake spec

rb-clean:
	cd ruby/ruby-watchman && bundle && bundle exec rake clean
else
rb-build:
rb-tests:
rb-clean:
endif

all-local: py-build rb-build lint

install-exec-local: py-install
if WATCHMAN_STATE_DIR
	${INSTALL} -d -m 777 ${DESTDIR}${WATCHMAN_STATE_DIR}
	chmod g+s ${DESTDIR}${WATCHMAN_STATE_DIR}
	touch ${DESTDIR}${WATCHMAN_STATE_DIR}/.not-empty
endif

clean-local: py-clean rb-clean
	-find python \( -name '*.py[cdo]' -o -name '*.so' \) -exec rm -f '{}' ';'
	-rm -rf python/build

# generate a rule that we can use to ensure that
# the test programs are built
build-tests: $(TESTS)
.PHONY: lint build-tests integration py-tests
# run integration AND unit tests
integration: all py-integration

tests_art_t_CPPFLAGS = $(THIRDPARTY_CPPFLAGS)
tests_art_t_LDADD = $(ART_LIB) $(TAP_LIB)
tests_art_t_SOURCES = \
	tests/art_test.c

tests_argv_t_CPPFLAGS = $(THIRDPARTY_CPPFLAGS)
tests_argv_t_LDADD = $(JSON_LIB) $(TAP_LIB)
tests_argv_t_SOURCES = \
	tests/argv.c \
	argv.c

tests_ignore_t_CPPFLAGS = $(THIRDPARTY_CPPFLAGS)
tests_ignore_t_LDADD = $(ART_LIB) $(TAP_LIB) $(JSON_LIB)
tests_ignore_t_SOURCES = \
	tests/ignore_test.c \
	watcher/helper.c \
	hash.c \
	ht.c \
	ignore.c \
	string.c \
	log.c

tests_log_t_CPPFLAGS = $(THIRDPARTY_CPPFLAGS)
tests_log_t_LDADD = $(JSON_LIB) $(TAP_LIB)
tests_log_t_SOURCES = \
	tests/log.c \
	log.c

tests_bser_t_CPPFLAGS = $(THIRDPARTY_CPPFLAGS)
tests_bser_t_LDADD = $(JSON_LIB) $(TAP_LIB)
tests_bser_t_SOURCES = \
	tests/bser.c \
	bser.c

tests_wildmatch_t_CPPFLAGS = $(THIRDPARTY_CPPFLAGS)
tests_wildmatch_t_LDADD = $(JSON_LIB) $(TAP_LIB) $(WILDMATCH_LIB)
tests_wildmatch_t_SOURCES = \
	tests/wildmatch_test.c

tests_critbit_t_CPPFLAGS = $(THIRDPARTY_CPPFLAGS)
tests_critbit_t_LDADD = $(TAP_LIB) $(CRITBIT_LIB)
tests_critbit_t_SOURCES = \
	tests/critbit_test.c

watch:
	PYTHONPATH=python python/bin/watchman-make \
			-p '**/*.[ch]' 'Makefile*' '**/*.py' '**/*.php' \
			-t integration
