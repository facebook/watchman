bin_PROGRAMS = watchman

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
	listener.c   \
	main.c       \
	root.c       \
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
TESTS = tests/argv.t
noinst_PROGRAMS = tests/argv.t

# generate a rule that we can use to ensure that
# the test programs are built
build-tests: $(TESTS)
.PHONY: build-tests integration
# run integration AND unit tests
integration:
	arc unit --engine WatchmanIntegrationEngine

tests_argv_t_CPPFLAGS = $(THIRDPARTY_CPPFLAGS)
tests_argv_t_LDADD = $(JSON_LIB) $(TAP_LIB)
tests_argv_t_SOURCES = \
	tests/argv.c \
	argv.c
