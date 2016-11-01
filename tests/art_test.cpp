/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0.
 * Derived from thirdparty/libart/tests/test_art.c which is
 * Copyright 2012 Armon Dadgar. See thirdparty/libart/LICENSE. */
#include "watchman_system.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>

#include "thirdparty/tap.h"
#include "thirdparty/libart/src/art.h"

#define stringy2(line)  #line
#define stringy(line)   stringy2(line)
#define fail_unless(__x)  ok(__x, __FILE__ ":" stringy(__LINE__) " " # __x)

static FILE *open_test_file(const char *name) {
  FILE *f = fopen(name, "r");
  char altname[1024];

  if (f) {
    return f;
  }

  snprintf(altname, sizeof(altname), "watchman/%s", name);
  f = fopen(altname, "r");
  if (f) {
    return f;
  }
  fail("can't find test data file %s", name);
  return NULL;
}

void test_art_insert(void) {
  art_tree<uintptr_t> t;
  int len;
  char buf[512];
  FILE *f = open_test_file("thirdparty/libart/tests/words.txt");
  uintptr_t line = 1;

  while (fgets(buf, sizeof buf, f)) {
    len = (int)strlen(buf);
    buf[len - 1] = '\0';
    t.insert((unsigned char*)buf, len, line);
    if (t.size() != line) {
      fail("art_size didn't match current line no");
    }
    line++;
  }
}

void test_art_insert_verylong(void) {
  art_tree<void*> t;

  unsigned char key1[300] = {
      16,  0,   0,   0,   7,   10,  0,   0,   0,   2,   17,  10,  0,   0,
      0,   120, 10,  0,   0,   0,   120, 10,  0,   0,   0,   216, 10,  0,
      0,   0,   202, 10,  0,   0,   0,   194, 10,  0,   0,   0,   224, 10,
      0,   0,   0,   230, 10,  0,   0,   0,   210, 10,  0,   0,   0,   206,
      10,  0,   0,   0,   208, 10,  0,   0,   0,   232, 10,  0,   0,   0,
      124, 10,  0,   0,   0,   124, 2,   16,  0,   0,   0,   2,   12,  185,
      89,  44,  213, 251, 173, 202, 211, 95,  185, 89,  110, 118, 251, 173,
      202, 199, 101, 0,   8,   18,  182, 92,  236, 147, 171, 101, 150, 195,
      112, 185, 218, 108, 246, 139, 164, 234, 195, 58,  177, 0,   8,   16,
      0,   0,   0,   2,   12,  185, 89,  44,  213, 251, 173, 202, 211, 95,
      185, 89,  110, 118, 251, 173, 202, 199, 101, 0,   8,   18,  180, 93,
      46,  151, 9,   212, 190, 95,  102, 178, 217, 44,  178, 235, 29,  190,
      218, 8,   16,  0,   0,   0,   2,   12,  185, 89,  44,  213, 251, 173,
      202, 211, 95,  185, 89,  110, 118, 251, 173, 202, 199, 101, 0,   8,
      18,  180, 93,  46,  151, 9,   212, 190, 95,  102, 183, 219, 229, 214,
      59,  125, 182, 71,  108, 180, 220, 238, 150, 91,  117, 150, 201, 84,
      183, 128, 8,   16,  0,   0,   0,   2,   12,  185, 89,  44,  213, 251,
      173, 202, 211, 95,  185, 89,  110, 118, 251, 173, 202, 199, 101, 0,
      8,   18,  180, 93,  46,  151, 9,   212, 190, 95,  108, 176, 217, 47,
      50,  219, 61,  134, 207, 97,  151, 88,  237, 246, 208, 8,   18,  255,
      255, 255, 219, 191, 198, 134, 5,   223, 212, 72,  44,  208, 250, 180,
      14,  1,   0,   0,   8,   '\0'};
  unsigned char key2[303] = {
      16,  0,   0,   0,   7,   10,  0,   0,   0,   2,   17,  10,  0,   0,   0,
      120, 10,  0,   0,   0,   120, 10,  0,   0,   0,   216, 10,  0,   0,   0,
      202, 10,  0,   0,   0,   194, 10,  0,   0,   0,   224, 10,  0,   0,   0,
      230, 10,  0,   0,   0,   210, 10,  0,   0,   0,   206, 10,  0,   0,   0,
      208, 10,  0,   0,   0,   232, 10,  0,   0,   0,   124, 10,  0,   0,   0,
      124, 2,   16,  0,   0,   0,   2,   12,  185, 89,  44,  213, 251, 173, 202,
      211, 95,  185, 89,  110, 118, 251, 173, 202, 199, 101, 0,   8,   18,  182,
      92,  236, 147, 171, 101, 150, 195, 112, 185, 218, 108, 246, 139, 164, 234,
      195, 58,  177, 0,   8,   16,  0,   0,   0,   2,   12,  185, 89,  44,  213,
      251, 173, 202, 211, 95,  185, 89,  110, 118, 251, 173, 202, 199, 101, 0,
      8,   18,  180, 93,  46,  151, 9,   212, 190, 95,  102, 178, 217, 44,  178,
      235, 29,  190, 218, 8,   16,  0,   0,   0,   2,   12,  185, 89,  44,  213,
      251, 173, 202, 211, 95,  185, 89,  110, 118, 251, 173, 202, 199, 101, 0,
      8,   18,  180, 93,  46,  151, 9,   212, 190, 95,  102, 183, 219, 229, 214,
      59,  125, 182, 71,  108, 180, 220, 238, 150, 91,  117, 150, 201, 84,  183,
      128, 8,   16,  0,   0,   0,   3,   12,  185, 89,  44,  213, 251, 133, 178,
      195, 105, 183, 87,  237, 150, 155, 165, 150, 229, 97,  182, 0,   8,   18,
      161, 91,  239, 50,  10,  61,  150, 223, 114, 179, 217, 64,  8,   12,  186,
      219, 172, 150, 91,  53,  166, 221, 101, 178, 0,   8,   18,  255, 255, 255,
      219, 191, 198, 134, 5,   208, 212, 72,  44,  208, 250, 180, 14,  1,   0,
      0,   8,   '\0'};

  t.insert(key1, 299, (void*)key1);
  t.insert(key2, 302, (void*)key2);
  t.insert(key2, 302, (void*)key2);
  fail_unless(t.size() == 2);
}

void test_art_insert_search(void) {
  art_tree<uintptr_t> t;
  int len;
  char buf[512];
  FILE *f = open_test_file("thirdparty/libart/tests/words.txt");
  uintptr_t line = 1;

  while (fgets(buf, sizeof buf, f)) {
    len = (int)strlen(buf);
    buf[len - 1] = '\0';
    t.insert((unsigned char*)buf, len, line);
    line++;
  }

  // Seek back to the start
  fseek(f, 0, SEEK_SET);

  // Search for each line
  line = 1;
  while (fgets(buf, sizeof buf, f)) {
    len = (int)strlen(buf);
    buf[len - 1] = '\0';

    {
      uintptr_t val = *t.search((unsigned char*)buf, len);
      if (line != val) {
        fail("Line: %d Val: %" PRIuPTR " Str: %s", line, val, buf);
      }
    }
    line++;
  }

  // Check the minimum
  auto l = t.minimum();
  fail_unless(l && strcmp((char *)l->key, "A") == 0);

  // Check the maximum
  l = t.maximum();
  fail_unless(l && strcmp((char *)l->key, "zythum") == 0);
}

void test_art_insert_delete(void) {
  art_tree<uintptr_t> t;
  int len;
  char buf[512];
  FILE *f = open_test_file("thirdparty/libart/tests/words.txt");

  uintptr_t line = 1, nlines;
  while (fgets(buf, sizeof buf, f)) {
    len = (int)strlen(buf);
    buf[len - 1] = '\0';
    t.insert((unsigned char*)buf, len, line);
    line++;
  }

  nlines = line - 1;

  // Seek back to the start
  fseek(f, 0, SEEK_SET);

  // Search for each line
  line = 1;
  while (fgets(buf, sizeof buf, f)) {
    uintptr_t val;
    len = (int)strlen(buf);
    buf[len - 1] = '\0';

    // Search first, ensure all entries still
    // visible
    val = *t.search((unsigned char*)buf, len);
    if (line != val) {
      fail("Line: %d Val: %" PRIuPTR " Str: %s", line, val, buf);
    }

    // Delete, should get lineno back
    if (!t.erase((unsigned char*)buf, len)) {
      fail("failed to erase line %d, str: %s", line, buf);
    }

    // Check the size
    if (t.size() != nlines - line) {
      fail("bad size after delete");
    }
    line++;
  }

  // Check the minimum and maximum
  fail_unless(!t.minimum());
  fail_unless(!t.maximum());
}

int iter_cb(void *data, const unsigned char *key, uint32_t key_len, void *val) {
  uint64_t *out = (uint64_t *)data;
  uintptr_t line = (uintptr_t)val;
  uint64_t mask = (line * (key[0] + key_len));
  out[0]++;
  out[1] ^= mask;
  return 0;
}

void test_art_insert_iter(void) {
  art_tree<void*> t;

  int len;
  char buf[512];
  FILE *f = open_test_file("thirdparty/libart/tests/words.txt");

  uint64_t xor_mask = 0;
  uintptr_t line = 1, nlines;
  while (fgets(buf, sizeof buf, f)) {
    len = (int)strlen(buf);
    buf[len - 1] = '\0';
    t.insert((unsigned char*)buf, len, (void*)line);

    xor_mask ^= (line * (buf[0] + len));
    line++;
  }
  nlines = line - 1;

  {
    uint64_t out[] = {0, 0};
    fail_unless(t.iter(iter_cb, &out) == 0);

    fail_unless(out[0] == nlines);
    fail_unless(out[1] == xor_mask);
  }
}

typedef struct {
  int count;
  int max_count;
  const char **expected;
} prefix_data;

template <typename T>
static int
test_prefix_cb(void* data, const unsigned char* k, uint32_t k_len, T&) {
  prefix_data *p = (prefix_data *)data;
  fail_unless(p->count < p->max_count);
  diag("Key: %s Expect: %s", k, p->expected[p->count]);
  fail_unless(memcmp(k, p->expected[p->count], k_len) == 0);
  p->count++;
  return 0;
}

void test_art_iter_prefix(void) {
  art_tree<void*> t;
  const char *s = "api.foo.bar";
  const char *expected2[] = {"abc.123.456", "api",         "api.foe.fum",
                             "api.foo",     "api.foo.bar", "api.foo.baz"};

  t.insert((unsigned char*)s, (int)strlen(s) + 1, nullptr);

  s = "api.foo.baz";
  t.insert((unsigned char*)s, (int)strlen(s) + 1, nullptr);

  s = "api.foe.fum";
  t.insert((unsigned char*)s, (int)strlen(s) + 1, nullptr);

  s = "abc.123.456";
  t.insert((unsigned char*)s, (int)strlen(s) + 1, nullptr);

  s = "api.foo";
  t.insert((unsigned char*)s, (int)strlen(s) + 1, nullptr);

  s = "api";
  t.insert((unsigned char*)s, (int)strlen(s) + 1, nullptr);

  {
    // Iterate over api
    const char *expected[] = {"api", "api.foe.fum", "api.foo", "api.foo.bar",
                              "api.foo.baz"};
    prefix_data p = {0, 5, expected};
    fail_unless(
        !t.iterPrefix((unsigned char*)"api", 3, test_prefix_cb<void*>, &p));
    diag("Count: %d Max: %d", p.count, p.max_count);
    fail_unless(p.count == p.max_count);
  }

  {
    // Iterate over 'a'
    prefix_data p2 = {0, 6, expected2};
    fail_unless(
        !t.iterPrefix((unsigned char*)"a", 1, test_prefix_cb<void*>, &p2));
    fail_unless(p2.count == p2.max_count);
  }

  {
    // Check a failed iteration
    prefix_data p3 = {0, 0, NULL};
    fail_unless(
        !t.iterPrefix((unsigned char*)"b", 1, test_prefix_cb<void*>, &p3));
    fail_unless(p3.count == 0);
  }

  {
    // Iterate over api.
    const char *expected4[] = {"api.foe.fum", "api.foo", "api.foo.bar",
                               "api.foo.baz"};
    prefix_data p4 = {0, 4, expected4};
    fail_unless(
        !t.iterPrefix((unsigned char*)"api.", 4, test_prefix_cb<void*>, &p4));
    diag("Count: %d Max: %d", p4.count, p4.max_count);
    fail_unless(p4.count == p4.max_count);
  }

  {
    // Iterate over api.foo.ba
    const char *expected5[] = {"api.foo.bar"};
    prefix_data p5 = {0, 1, expected5};
    fail_unless(!t.iterPrefix(
        (unsigned char*)"api.foo.bar", 11, test_prefix_cb<void*>, &p5));
    diag("Count: %d Max: %d", p5.count, p5.max_count);
    fail_unless(p5.count == p5.max_count);
  }

  // Check a failed iteration on api.end
  {
    prefix_data p6 = {0, 0, NULL};
    fail_unless(!t.iterPrefix(
        (unsigned char*)"api.end", 7, test_prefix_cb<void*>, &p6));
    fail_unless(p6.count == 0);
  }

  // Iterate over empty prefix
  {
    prefix_data p7 = {0, 6, expected2};
    fail_unless(
        !t.iterPrefix((unsigned char*)"", 0, test_prefix_cb<void*>, &p7));
    fail_unless(p7.count == p7.max_count);
  }
}

void test_art_long_prefix(void) {
  art_tree<uintptr_t> t;
  uintptr_t v;
  const char *s;

  s = "this:key:has:a:long:prefix:3";
  v = 3;
  t.insert((unsigned char*)s, (int)strlen(s) + 1, v);

  s = "this:key:has:a:long:common:prefix:2";
  v = 2;
  t.insert((unsigned char*)s, (int)strlen(s) + 1, v);

  s = "this:key:has:a:long:common:prefix:1";
  v = 1;
  t.insert((unsigned char*)s, (int)strlen(s) + 1, v);

  // Search for the keys
  s = "this:key:has:a:long:common:prefix:1";
  fail_unless(1 == *t.search((unsigned char*)s, (int)strlen(s) + 1));

  s = "this:key:has:a:long:common:prefix:2";
  fail_unless(2 == *t.search((unsigned char*)s, (int)strlen(s) + 1));

  s = "this:key:has:a:long:prefix:3";
  fail_unless(3 == *t.search((unsigned char*)s, (int)strlen(s) + 1));

  {
    const char *expected[] = {
        "this:key:has:a:long:common:prefix:1",
        "this:key:has:a:long:common:prefix:2", "this:key:has:a:long:prefix:3",
    };
    prefix_data p = {0, 3, expected};
    fail_unless(!t.iterPrefix(
        (unsigned char*)"this:key:has", 12, test_prefix_cb<uintptr_t>, &p));
    diag("Count: %d Max: %d", p.count, p.max_count);
    fail_unless(p.count == p.max_count);
  }
}

static int dump_iter(void *data, const unsigned char *key, unsigned int key_len,
                     void *value) {
  diag("iter leaf: data=%p key_len=%d %.*s value=%p", data, (int)key_len,
       (int)key_len, key, value);
  return 0;
}

void test_art_prefix(void) {
  art_tree<void*> t;
  void *v;

  t.insert((const unsigned char*)"food", 4, (void*)"food");
  t.insert((const unsigned char*)"foo", 3, (void*)"foo");
  diag("size is now %d", t.size());
  fail_unless(t.size() == 2);
  fail_unless((v = *t.search((const unsigned char*)"food", 4)) != NULL);
  diag("food lookup yields %s", v);
  fail_unless(v && strcmp((char*)v, "food") == 0);

  t.iter(dump_iter, NULL);

  fail_unless((v = *t.search((const unsigned char*)"foo", 3)) != NULL);
  diag("foo lookup yields %s", v);
  fail_unless(v && strcmp((char*)v, "foo") == 0);
}

void test_art_insert_search_uuid(void) {
  art_tree<uintptr_t> t;
  int len;
  char buf[512];
  FILE *f = open_test_file("thirdparty/libart/tests/uuid.txt");
  uintptr_t line = 1;

  while (fgets(buf, sizeof buf, f)) {
    len = (int)strlen(buf);
    buf[len - 1] = '\0';
    t.insert((unsigned char*)buf, len, line);
    line++;
  }

  // Seek back to the start
  fseek(f, 0, SEEK_SET);

  // Search for each line
  line = 1;
  while (fgets(buf, sizeof buf, f)) {
    uintptr_t val;
    len = (int)strlen(buf);
    buf[len - 1] = '\0';

    val = *t.search((unsigned char*)buf, len);
    if (line != val) {
      fail("Line: %d Val: %" PRIuPTR " Str: %s\n", line, val, buf);
    }
    line++;
  }

  // Check the minimum
  auto l = t.minimum();
  diag("minimum is %s", l->key);
  fail_unless(
      l && strcmp((char *)l->key, "00026bda-e0ea-4cda-8245-522764e9f325") == 0);

  // Check the maximum
  l = t.maximum();
  diag("maximum is %s", l->key);
  fail_unless(
      l && strcmp((char *)l->key, "ffffcb46-a92e-4822-82af-a7190f9c1ec5") == 0);
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  plan_tests(84);
  test_art_insert();
  test_art_insert_verylong();
  test_art_insert_search();
  test_art_insert_delete();
  test_art_insert_iter();
  test_art_iter_prefix();
  test_art_long_prefix();
  test_art_insert_search_uuid();
  test_art_prefix();

  return exit_status();
}
