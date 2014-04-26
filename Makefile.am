bin_PROGRAMS = watchman

doc_DATA = README.markdown
docdir = ${prefix}/share/doc/watchman-$(VERSION)

THIRDPARTY_CPPFLAGS = -I$(top_srcdir)/thirdparty/jansson -I$(top_builddir)/thirdparty/jansson
JSON_LIB = -L. libwmanjson.a

watchman_CPPFLAGS = $(THIRDPARTY_CPPFLAGS) @IRONMANCFLAGS@
watchman_LDADD = $(JSON_LIB)
watchman_SOURCES = \
	argv.c       \
	envp.c       \
	spawn.c       \
	opt.c        \
	cfg.c        \
	log.c        \
	json.c       \
	bser.c       \
	hash.c       \
	ht.c         \
	cmds/find.c     \
	cmds/info.c     \
	cmds/log.c      \
	cmds/query.c    \
	cmds/since.c    \
	cmds/subscribe.c    \
	cmds/trigger.c  \
	cmds/watch.c    \
	cmds/debug.c    \
	query/base.c       \
	query/parse.c      \
	query/init.c       \
	query/eval.c       \
	query/type.c       \
	query/suffix.c     \
	query/match.c      \
	query/pcre.c       \
	query/name.c       \
	query/fieldlist.c  \
	query/since.c      \
	query/empty.c      \
	listener.c   \
	clientmode.c \
	main.c       \
	root.c       \
	state.c      \
	string.c

noinst_HEADERS =   \
	watchman.h       \
	watchman_hash.h

noinst_LIBRARIES = libwmanjson.a libtap.a

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

# unit tests
TESTS = tests/argv.t tests/log.t tests/bser.t
noinst_PROGRAMS = tests/argv.t tests/log.t tests/bser.t

if HAVE_ARC
# if we have arc, lint when we run make.  We don't want to force
# folks to have arc just to build it and install it, which should
# be the much more common case than those developing it!
all-local: lint

# Run lint and output stuff suitable for feeding into ":make" in vim
# quickfix mode.
lint:
	-arc lint --output compiler --lintall
endif

if HAVE_PYTHON
all-local:
	cd python/bser && $(PYTHON) ./setup.py build

# This is invoked via WatchmanIntegrationEngine
py-tests:
	time $(PYTHON) $(TESTNAME)

clean-local:
	cd python/bser && $(PYTHON) ./setup.py clean
	rm -rf python/bser/build
else
py-tests:
endif

# generate a rule that we can use to ensure that
# the test programs are built
build-tests: $(TESTS)
.PHONY: lint build-tests integration py-tests
# run integration AND unit tests
integration:
	arc unit --everything

tests_argv_t_CPPFLAGS = $(THIRDPARTY_CPPFLAGS)
tests_argv_t_LDADD = $(JSON_LIB) $(TAP_LIB)
tests_argv_t_SOURCES = \
	tests/argv.c \
	argv.c

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

