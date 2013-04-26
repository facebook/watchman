bin_PROGRAMS = watchman

doc_DATA = README.markdown
docdir = ${prefix}/share/doc/watchman-$(VERSION)

THIRDPARTY_CPPFLAGS = -Ithirdparty/jansson
JSON_LIB = -L. -lwmanjson

watchman_CPPFLAGS = $(THIRDPARTY_CPPFLAGS) @IRONMANCFLAGS@
watchman_LDADD = $(JSON_LIB)
watchman_SOURCES = \
	argv.c       \
	opt.c        \
	log.c        \
	json.c       \
	hash.c       \
	ht.c         \
	cmds/find.c     \
	cmds/log.c      \
	cmds/query.c    \
	cmds/since.c    \
	cmds/subscribe.c    \
	cmds/trigger.c  \
	cmds/watch.c    \
	cmds/version.c    \
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
	listener.c   \
	clientmode.c \
	main.c       \
	root.c       \
	rules.c      \
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
TAP_LIB = -ltap

# unit tests
TESTS = tests/argv.t tests/log.t
noinst_PROGRAMS = tests/argv.t tests/log.t

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

# generate a rule that we can use to ensure that
# the test programs are built
build-tests: $(TESTS)
.PHONY: lint build-tests integration
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

