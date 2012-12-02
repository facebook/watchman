bin_PROGRAMS = watchman

watchman_SOURCES = \
	argv.c       \
	hash.c       \
	ht.c         \
	listener.c   \
	main.c       \
	root.c       \
	string.c

noinst_HEADERS =   \
	watchman.h       \
	watchman_hash.h

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

tests_argv_t_SOURCES = \
	thirdparty/tap.c  \
	tests/argv.c \
	argv.c
