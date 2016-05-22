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

void w_request_shutdown(void) {}

bool w_should_log_to_clients(int level)
{
  unused_parameter(level);
  return false;
}

void w_log_to_clients(int level, const char *buf)
{
  unused_parameter(level);
  unused_parameter(buf);
}

struct ignore_state {
  w_ht_t *ignore_dirs;
  w_ht_t *ignore_vcs;
};

struct test_case {
  const char *path;
  bool ignored;
};

bool check_ignores_iter(struct ignore_state *state, const char *path,
                        uint32_t pathlen) {
  if (w_check_ignores(state->ignore_dirs, path, pathlen)) {
    return true;
  }

  return w_check_vcs_ignores(state->ignore_vcs, path, pathlen);
}

// A follow-on diff will add an alternative checker function, hence
// this implementation has parameterized the checker even though there
// is only a single implementation right now.
void run_correctness_test(struct ignore_state *state,
                          const struct test_case *tests, uint32_t num_tests,
                          bool (*checker)(struct ignore_state *, const char *,
                                          uint32_t)) {

  uint32_t i;

  for (i = 0; i < num_tests; i++) {
    bool res = checker(state, tests[i].path, strlen_uint32(tests[i].path));
    ok(res == tests[i].ignored, "%s expected=%d actual=%d", tests[i].path,
       tests[i].ignored, res);
  }
}

void add_strings(w_ht_t *ht, const char **strings, uint32_t num_strings) {
  uint32_t i;

  for (i = 0; i < num_strings; i++) {
    w_string_t *str = w_string_new(strings[i]);
    w_ht_set(ht, w_ht_ptr_val(str), w_ht_ptr_val(str));
  }
}

void init_state(struct ignore_state *state) {
  state->ignore_dirs = w_ht_new(2, &w_ht_string_funcs);
  state->ignore_vcs = w_ht_new(2, &w_ht_string_funcs);

  add_strings(state->ignore_dirs, ignore_dirs,
              sizeof(ignore_dirs) / sizeof(ignore_dirs[0]));

  add_strings(state->ignore_vcs, ignore_vcs,
              sizeof(ignore_vcs) / sizeof(ignore_vcs[0]));
}

void test_correctness(void) {
  struct ignore_state state;

  init_state(&state);

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

  run_correctness_test(&state, tests, sizeof(tests) / sizeof(tests[0]),
                       check_ignores_iter);
}

// Load up the words data file and build a list of strings from that list.
// Each of those strings is prefixed with the supplied string.
// If there are fewer than limit entries available in the data file, we will
// abort.
w_string_t** build_list_with_prefix(const char *prefix, size_t limit) {
  w_string_t **strings = calloc(limit, sizeof(w_string_t*));
  char buf[512];
  FILE *f = fopen("thirdparty/libart/tests/words.txt", "r");
  size_t i = 0;

  while (fgets(buf, sizeof buf, f)) {
    // Remove newline
    uint32_t len = strlen_uint32(buf);
    buf[len - 1] = '\0';
    strings[i++] = w_string_make_printf("%s%s", prefix, buf);

    if (i >= limit) {
      break;
    }
  }

  if (i < limit) {
    abort();
  }

  return strings;
}

static const size_t kWordLimit = 230000;

void bench_list(const char *label, const char *prefix,
                bool (*checker)(struct ignore_state *, const char *,
                                uint32_t)) {

  struct ignore_state state;

  init_state(&state);
  size_t i, n;
  w_string_t **strings = build_list_with_prefix(prefix, kWordLimit);

  struct timeval start;
  gettimeofday(&start, NULL);
  for (n = 0; n < 100; n++) {
    for (i = 0; i < kWordLimit; i++) {
      checker(&state, strings[i]->buf, strings[i]->len);
    }
  }
  struct timeval end;
  gettimeofday(&end, NULL);

  diag("%s: took %.3fs", label, w_timeval_diff(start, end));
}

void bench_all_ignores(void) {
  bench_list("all_ignores_iter", "baz/buck-out/gen/", check_ignores_iter);
}

void bench_no_ignores(void) {
  bench_list("no_ignores_iter", "baz/some/path", check_ignores_iter);
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  plan_tests(17);
  test_correctness();
  bench_all_ignores();
  bench_no_ignores();

  return exit_status();
}
