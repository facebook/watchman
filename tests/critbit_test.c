/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */

#include "thirdparty/tap.h"
#include "thirdparty/critbit.h"
#include <string.h>

static void test_basic_simple(void) {
  cb_tree_t tree = cb_tree_make();

  ok(cb_tree_getitem(&tree, "foo") == NULL, "no foo in tree yet");

  ok(!cb_tree_contains(&tree, "foo"), "no foo");
  ok(cb_tree_setitem(&tree, "foo", "1", NULL) == 0, "stored foo -> 1");
  ok(cb_tree_contains(&tree, "foo"), "contains foo");

  ok(cb_tree_setdefault(&tree, "foo", "wat", NULL) == -1,
     "setdefault can't replace existing value");

  ok(!cb_tree_contains(&tree, "bar"), "no bar");
  ok(cb_tree_setdefault(&tree, "bar", "2", NULL) == 0, "stored bar -> 2");
  ok(cb_tree_contains(&tree, "bar"), "contains bar");

  ok(tree.count == 2, "2 elements, have %d", tree.count);

  ok(!strcmp(cb_tree_getitem(&tree, "foo"), "1"), "should have gotten 1");
  ok(!strcmp(cb_tree_getitem(&tree, "bar"), "2"), "should have gotten 2");

  ok(cb_tree_setitem(&tree, "foo", "3", NULL) == 1, "stored foo -> 3");
  ok(tree.count == 2, "still have 2 elements, have %d", tree.count);
  ok(!strcmp(cb_tree_getitem(&tree, "foo"), "3"), "should have gotten 3");

  ok(cb_tree_delete(&tree, "bar", NULL, NULL) == 0, "removed bar");
  ok(tree.count == 1, "now have 1 element, have %d", tree.count);
  ok(!strcmp(cb_tree_getitem(&tree, "foo"), "3"), "should have gotten 3");
  ok(cb_tree_getitem(&tree, "bar") == NULL, "bar should be gone");

  cb_tree_clear(&tree);
  ok(tree.count == 0, "no more entries");
}

static void test_basic_popitem(void) {
  cb_tree_t tree = cb_tree_make();
  const char *key, *value;

  ok(cb_tree_setitem(&tree, "foo1", "1", NULL) == 0, "stored foo1");
  ok(cb_tree_setitem(&tree, "foo2", "2", NULL) == 0, "stored foo2");
  ok(cb_tree_setitem(&tree, "foo12", "3", NULL) == 0, "stored foo12");

  // Note the order of the pops
  ok(cb_tree_popitem(&tree, (const void **)&key, (const void **)&value) == 0,
     "popped");
  ok(!strcmp(key, "foo1"), "foo1");
  ok(!strcmp(value, "1"), "value: 1");

  ok(cb_tree_popitem(&tree, (const void **)&key, (const void **)&value) == 0,
     "popped");
  ok(!strcmp(key, "foo12"), "foo12");
  ok(!strcmp(value, "3"), "value: 3");

  ok(cb_tree_popitem(&tree, (const void **)&key, (const void **)&value) == 0,
     "popped");
  ok(!strcmp(key, "foo2"), "foo2");
  ok(!strcmp(value, "2"), "value: 2");

  ok(tree.count == 0, "no more entries");
  cb_tree_clear(&tree);
}

static void test_basic_has_prefix(void) {
  cb_tree_t tree = cb_tree_make();

  ok(cb_tree_has_prefix_str(&tree, "", 0) == 0,
     "doesn't match empty string prefix");
  ok(cb_tree_has_prefix_str(&tree, "foo", 3) == 0, "doesn't match foo prefix");

  ok(cb_tree_setitem(&tree, "foo1", "1", NULL) == 0, "inserted foo1");

  ok(cb_tree_has_prefix_str(&tree, "", 0) == 1, "matches empty prefix");
  ok(cb_tree_has_prefix_str(&tree, "foo", 3) == 1, "matches foo prefix");
  ok(cb_tree_has_prefix_str(&tree, "foo1", 4) == 1, "matches foo1 prefix");
  ok(cb_tree_has_prefix_str(&tree, "foo12", 5) == 0, "no foo12 prefix");
  ok(cb_tree_has_prefix_str(&tree, "foo2", 4) == 0, "no foo2 prefix");

  ok(cb_tree_setitem(&tree, "foo12", "2", NULL) == 0, "inserted foo12");

  ok(cb_tree_has_prefix_str(&tree, "", 0) == 1, "matches empty prefix");
  ok(cb_tree_has_prefix_str(&tree, "foo", 3) == 1, "matches foo prefix");
  ok(cb_tree_has_prefix_str(&tree, "foo1", 4) == 1, "matches foo1 prefix");
  ok(cb_tree_has_prefix_str(&tree, "foo12", 5) == 1, "matches foo12 prefix");
  ok(cb_tree_has_prefix_str(&tree, "foo2", 4) == 0, "no foo2 prefix");

  cb_tree_clear(&tree);
}

static void test_basic_iter(void) {
  cb_tree_t tree = cb_tree_make();
  cb_iter_t iter;
  const char *key, *value;

  ok(cb_tree_setitem(&tree, "foo1", "1", NULL) == 0, "stored foo1");
  ok(cb_tree_setitem(&tree, "foo2", "2", NULL) == 0, "stored foo2");
  ok(cb_tree_setitem(&tree, "foo12", "3", NULL) == 0, "stored foo12");

  iter = cb_tree_iter(&tree);

  ok(cb_tree_iter_next(&iter, (void **)&key, NULL) == 1, "iter 1");
  ok(!strcmp(key, "foo1"), "foo1");

  ok(cb_tree_iter_next(&iter, (void **)&key, NULL) == 1, "iter 2");
  ok(!strcmp(key, "foo12"), "foo12");

  ok(cb_tree_iter_next(&iter, (void **)&key, NULL) == 1, "iter 3");
  ok(!strcmp(key, "foo2"), "foo2");

  ok(cb_tree_iter_next(&iter, (void **)&key, NULL) == 0, "no more items");

  // Run through against to verify the values
  iter = cb_tree_iter(&tree);

  ok(cb_tree_iter_next(&iter, NULL, (void **)&value) == 1, "iter 1");
  ok(!strcmp(value, "1"), "foo1");

  ok(cb_tree_iter_next(&iter, NULL, (void **)&value) == 1, "iter 2");
  ok(!strcmp(value, "3"), "foo12");

  ok(cb_tree_iter_next(&iter, NULL, (void **)&value) == 1, "iter 3");
  ok(!strcmp(value, "2"), "foo2");

  ok(cb_tree_iter_next(&iter, (void **)&key, NULL) == 0, "no more items");

  // Now test that prefix iteration works
  ok(cb_tree_setitem(&tree, "bar", "b", NULL) == 0, "stored bar");

  iter = cb_tree_iter_prefix_str(&tree, "foo", 3);

  ok(cb_tree_iter_next(&iter, (void **)&key, NULL) == 1, "iter 1");
  ok(!strcmp(key, "foo1"), "foo1");

  ok(cb_tree_iter_next(&iter, (void **)&key, NULL) == 1, "iter 2");
  ok(!strcmp(key, "foo12"), "foo12");

  ok(cb_tree_iter_next(&iter, (void **)&key, NULL) == 1, "iter 3");
  ok(!strcmp(key, "foo2"), "foo2");

  ok(cb_tree_iter_next(&iter, (void **)&key, NULL) == 0, "no more items");

  cb_tree_clear(&tree);
}

static void test_longest_prefix(void) {
  cb_tree_t tree = cb_tree_make();
  struct {
    const char *path;
    const char *value;
  } defaults[] = {
      {"/Users/wez/src", "t"},
      {"/Users/wez/srd", "a"},
      {"/Users/wez/src/buck-out", "f"},
      {"/Users/wez/src/buck-outa", "a"},
      {"/Users/wez/src/buck-outb", "b"},
      {"/Users/wez/src/buck-out/lemona", "x"},
  };

  struct {
    const char *input_path;
    int expected_result;
    const char *expected_value;
  } expected[] = {
      {"/Users/wez/src", 14, "t"},
      {"/Users/wez/src/foo.c", 14, "t"},
      {"/", 0, NULL},
      {"", 0, NULL},
      {"/Users/wez/src/buck-out", 23, "f"},
      {"/Users/wez/src/buck-out/lemon", 23, "f"},
      {"/Users/wez/srce", 14, "t"},
      {"/Users/wez/srd", 14, "a"},
      {"/Users/wez/srb", 0, NULL},
  };
  uint32_t i;

  for (i = 0; i < sizeof(defaults)/sizeof(defaults[0]); i++) {
    ok(cb_tree_setitem(&tree, (void *)defaults[i].path,
                       (void *)defaults[i].value, NULL) == 0,
       "inserted");
  }

  for (i = 0; i < sizeof(expected)/sizeof(expected[0]); i++) {
    int inlen = strlen(expected[i].input_path);
    const char *value = NULL;
    ssize_t ret = cb_tree_longest_match(&tree, expected[i].input_path, inlen,
                                        (void **)&value);

    ok(ret == expected[i].expected_result, "input %s ret %d == expected %d",
       expected[i].input_path, ret, expected[i].expected_result);

    if (expected[i].expected_value) {
      ok(!strcmp(expected[i].expected_value, value),
         "input %s value %s == expected %s", expected[i].input_path, value,
         expected[i].expected_value);
    }
  }

  cb_tree_clear(&tree);
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  plan_tests(92);
  test_basic_simple();
  test_basic_popitem();
  test_basic_has_prefix();
  test_basic_iter();
  test_longest_prefix();

  return exit_status();
}

/* vim:ts=2:sw=2:et:
 */
