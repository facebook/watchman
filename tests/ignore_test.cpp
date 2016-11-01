/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0. */

#include "watchman.h"
#include "thirdparty/tap.h"

// A list that looks similar to one used in one of our repos
const char *ignore_dirs[] = {".buckd",
                             ".idea",
                             "_build",
                             "buck-cache",
                             "buck-out",
                             "build",
                             "foo/.buckd",
                             "foo/buck-cache",
                             "foo/buck-out",
                             "bar/_build",
                             "bar/buck-cache",
                             "bar/buck-out",
                             "baz/.buckd",
                             "baz/buck-cache",
                             "baz/buck-out",
                             "baz/build",
                             "baz/qux",
                             "baz/focus-out",
                             "baz/tmp",
                             "baz/foo/bar/foo/build",
                             "baz/foo/bar/bar/build",
                             "baz/foo/bar/baz/build",
                             "baz/foo/bar/qux",
                             "baz/foo/baz/foo",
                             "baz/bar/foo/foo/foo/foo/foo/foo",
                             "baz/bar/bar/foo/foo",
                             "baz/bar/bar/foo/foo"};

const char *ignore_vcs[] = {".hg", ".svn", ".git"};

struct test_case {
  const char *path;
  bool ignored;
};

void run_correctness_test(
    struct watchman_ignore* state,
    const struct test_case* tests,
    uint32_t num_tests) {
  uint32_t i;

  for (i = 0; i < num_tests; i++) {
    bool res = state->isIgnored(tests[i].path, strlen_uint32(tests[i].path));
    ok(res == tests[i].ignored, "%s expected=%d actual=%d", tests[i].path,
       tests[i].ignored, res);
  }
}

void add_strings(struct watchman_ignore *ignore, const char **strings,
                 uint32_t num_strings, bool is_vcs_ignore) {
  uint32_t i;
  for (i = 0; i < num_strings; i++) {
    ignore->add(w_string(strings[i], W_STRING_UNICODE), is_vcs_ignore);
  }
}

void init_state(struct watchman_ignore* state) {
  add_strings(state, ignore_dirs, sizeof(ignore_dirs) / sizeof(ignore_dirs[0]),
              false);

  add_strings(state, ignore_vcs, sizeof(ignore_vcs) / sizeof(ignore_vcs[0]),
              true);
}

void test_correctness(void) {
  struct watchman_ignore state;
  static const struct test_case tests[] = {
    {"some/path", false},
    {"buck-out/gen/foo", true},
    {".hg/wlock", false},
    {".hg/store/foo", true},
    {"buck-out", true},
    {"foo/buck-out", true},
    {"foo/hello", false},
    {"baz/hello", false},
    {".hg", false},
    {"buil", false},
    {"build", true},
    {"build/lower", true},
    {"builda", false},
    {"build/bar", true},
    {"buildfile", false},
    {"build/lower/baz", true},
    {"builda/hello", false},
  };

  init_state(&state);

  run_correctness_test(&state, tests, sizeof(tests) / sizeof(tests[0]));
}

// Load up the words data file and build a list of strings from that list.
// Each of those strings is prefixed with the supplied string.
// If there are fewer than limit entries available in the data file, we will
// abort.
std::vector<w_string> build_list_with_prefix(const char* prefix, size_t limit) {
  std::vector<w_string> strings;
  char buf[512];
  FILE *f = fopen("thirdparty/libart/tests/words.txt", "r");

  if (!f) {
    f = fopen("watchman/thirdparty/libart/tests/words.txt", "r");
  }

  while (fgets(buf, sizeof buf, f)) {
    // Remove newline
    uint32_t len = strlen_uint32(buf);
    buf[len - 1] = '\0';
    strings.emplace_back(w_string::printf("%s%s", prefix, buf));

    if (strings.size() >= limit) {
      break;
    }
  }

  if (strings.size() < limit) {
    abort();
  }

  return strings;
}

static const size_t kWordLimit = 230000;

void bench_list(const char* label, const char* prefix) {
  struct watchman_ignore state;
  size_t i, n;
  struct timeval start, end;

  init_state(&state);
  auto strings = build_list_with_prefix(prefix, kWordLimit);

  gettimeofday(&start, NULL);
  for (n = 0; n < 100; n++) {
    for (i = 0; i < kWordLimit; i++) {
      state.isIgnored(strings[i].data(), strings[i].size());
    }
  }
  gettimeofday(&end, NULL);

  diag("%s: took %.3fs", label, w_timeval_diff(start, end));
}

void bench_all_ignores(void) {
  bench_list("all_ignores_tree", "baz/buck-out/gen/");
}

void bench_no_ignores(void) {
  bench_list("no_ignores_tree", "baz/some/path");
}

int main(int, char**) {
  plan_tests(17);
  test_correctness();
  bench_all_ignores();
  bench_no_ignores();

  return exit_status();
}
