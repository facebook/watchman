bin_PROGRAMS = watchman
# ensure that we have a reasonable default for the python install
DESTDIR ?= /

doc_DATA = README.markdown
docdir = ${prefix}/share/doc/watchman-$(VERSION)

THIRDPARTY_CPPFLAGS = -I$(top_srcdir)/thirdparty/jansson -I$(top_builddir)/thirdparty/jansson
JSON_LIB = -L. libwmanjson.a

watchman_CPPFLAGS = $(THIRDPARTY_CPPFLAGS) @IRONMANCFLAGS@
watchman_LDADD = $(JSON_LIB) libwildmatch.a
watchman_SOURCES = \
	CookieSync.cpp \
	InMemoryView.cpp \
	QueryableView.cpp \
	argv.cpp       \
	envp.cpp       \
	spawn.cpp       \
	opt.cpp        \
	cfg.cpp        \
	clockspec.cpp  \
	checksock.cpp  \
	fstype.cpp     \
	log.cpp        \
	ignore.cpp     \
	json.cpp       \
	bser.cpp       \
	expflags.cpp   \
	hash.cpp       \
	ioprio.cpp        \
	opendir.cpp       \
	pending.cpp       \
	perf.cpp          \
	stream.cpp        \
	stream_stdout.cpp \
	stream_unix.cpp   \
	timedlock.cpp     \
	cmds/find.cpp     \
	cmds/info.cpp     \
	cmds/log.cpp      \
	cmds/query.cpp    \
	cmds/since.cpp    \
	cmds/reg.cpp      \
	cmds/state.cpp    \
	cmds/subscribe.cpp    \
	cmds/trigger.cpp  \
	cmds/watch.cpp    \
	cmds/debug.cpp    \
	query/base.cpp       \
	query/dirname.cpp    \
	query/parse.cpp      \
	query/eval.cpp       \
	query/glob.cpp       \
	query/intcompare.cpp \
	query/type.cpp       \
	query/suffix.cpp     \
	query/match.cpp      \
	query/pcre.cpp       \
	query/name.cpp       \
	query/fieldlist.cpp  \
	query/since.cpp      \
	query/empty.cpp      \
	watcher/auto.cpp     \
	watcher/fsevents.cpp \
	watcher/inotify.cpp  \
	watcher/kqueue.cpp   \
	watcher/portfs.cpp   \
	launchd.cpp    \
	listener.cpp   \
	listener-user.cpp   \
	clientmode.cpp \
	main.cpp       \
	root/ageout.cpp       \
	root/crawler.cpp       \
	root/dir.cpp       \
	root/file.cpp       \
	root/init.cpp       \
	root/iothread.cpp       \
	root/lock.cpp       \
	root/notifythread.cpp       \
	root/poison.cpp       \
	root/reap.cpp       \
	root/resolve.cpp       \
	root/stat.cpp       \
	root/symlink.cpp       \
	root/sync.cpp       \
	root/threading.cpp       \
	root/vcs.cpp       \
	root/warnerr.cpp       \
	root/watchlist.cpp       \
	state.cpp      \
	string.cpp     \
	time.cpp

noinst_HEADERS =   \
	watchman.h       \
	watchman_hash.h

noinst_LIBRARIES = \
	libtap.a \
	libwildmatch.a \
	libwmanjson.a

# bundled json library
libwmanjson_a_CPPFLAGS = $(THIRDPARTY_CPPFLAGS) @IRONMANCFLAGS@
libwmanjson_a_SOURCES = \
	thirdparty/jansson/dump.cpp \
	thirdparty/jansson/error.cpp \
	thirdparty/jansson/jansson_private.h \
	thirdparty/jansson/load.cpp \
	thirdparty/jansson/memory.cpp \
	thirdparty/jansson/pack_unpack.cpp \
	thirdparty/jansson/strbuffer.cpp \
	thirdparty/jansson/strbuffer.h \
	thirdparty/jansson/strconv.cpp \
	thirdparty/jansson/utf.cpp \
	thirdparty/jansson/utf.h \
	thirdparty/jansson/value.cpp

# bundled testing library
libtap_a_CPPFLAGS = $(THIRDPARTY_CPPFLAGS) @IRONMANCFLAGS@
libtap_a_SOURCES = thirdparty/tap.c
TAP_LIB = libtap.a

# pattern matching library
libwildmatch_a_SOURCES = thirdparty/wildmatch/wildmatch.c
WILDMATCH_LIB = libwildmatch.a

# unit tests
TESTS = \
		tests/art.t \
		tests/argv.t \
		tests/bser.t \
		tests/ignore.t \
		tests/pending.t \
		tests/string.t \
		tests/log.t \
		tests/wildmatch.t
noinst_PROGRAMS = $(TESTS)

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

all-local: py-build rb-build

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
.PHONY: build-tests integration py-tests
# run integration AND unit tests
integration: all py-integration check

tests_art_t_CPPFLAGS = $(THIRDPARTY_CPPFLAGS) @IRONMANCFLAGS@
tests_art_t_LDADD = $(TAP_LIB)
tests_art_t_SOURCES = \
	tests/art_test.cpp \
	tests/log_stub.cpp \
	log.cpp

tests_argv_t_CPPFLAGS = $(THIRDPARTY_CPPFLAGS) @IRONMANCFLAGS@
tests_argv_t_LDADD = $(JSON_LIB) $(TAP_LIB)
tests_argv_t_SOURCES = \
	tests/argv.cpp \
	tests/log_stub.cpp \
	argv.cpp \
	string.cpp \
	hash.cpp \
	log.cpp

tests_ignore_t_CPPFLAGS = $(THIRDPARTY_CPPFLAGS) @IRONMANCFLAGS@
tests_ignore_t_LDADD = $(TAP_LIB) $(JSON_LIB)
tests_ignore_t_SOURCES = \
	tests/ignore_test.cpp \
	tests/log_stub.cpp \
	hash.cpp \
	ignore.cpp \
	string.cpp \
	log.cpp

tests_string_t_CPPFLAGS = $(THIRDPARTY_CPPFLAGS) @IRONMANCFLAGS@
tests_string_t_LDADD = $(TAP_LIB) $(JSON_LIB)
tests_string_t_SOURCES = \
	tests/string_test.cpp \
	tests/log_stub.cpp \
	hash.cpp \
	string.cpp \
	log.cpp

tests_pending_t_CPPFLAGS = $(THIRDPARTY_CPPFLAGS) @IRONMANCFLAGS@
tests_pending_t_LDADD = $(TAP_LIB) $(JSON_LIB)
tests_pending_t_SOURCES = \
	tests/pending_test.cpp \
	tests/log_stub.cpp \
	hash.cpp \
	ignore.cpp \
	pending.cpp \
	expflags.cpp \
	opendir.cpp \
	cfg.cpp \
	time.cpp \
	string.cpp \
	log.cpp

tests_log_t_CPPFLAGS = $(THIRDPARTY_CPPFLAGS) @IRONMANCFLAGS@
tests_log_t_LDADD = $(JSON_LIB) $(TAP_LIB)
tests_log_t_SOURCES = \
	tests/log.cpp \
	log.cpp

tests_bser_t_CPPFLAGS = $(THIRDPARTY_CPPFLAGS) @IRONMANCFLAGS@
tests_bser_t_LDADD = $(JSON_LIB) $(TAP_LIB)
tests_bser_t_SOURCES = \
	tests/bser.cpp \
	tests/log_stub.cpp \
	bser.cpp \
	string.cpp \
	hash.cpp \
	log.cpp

tests_wildmatch_t_CPPFLAGS = $(THIRDPARTY_CPPFLAGS) @IRONMANCFLAGS@
tests_wildmatch_t_LDADD = $(JSON_LIB) $(TAP_LIB) $(WILDMATCH_LIB)
tests_wildmatch_t_SOURCES = \
	tests/wildmatch_test.cpp \
	tests/log_stub.cpp \
	string.cpp \
	hash.cpp \
	log.cpp

watch:
	PYTHONPATH=python python/bin/watchman-make \
			-p '**/*.[ch]' 'Makefile*' '**/*.py' '**/*.php' \
			-t integration
