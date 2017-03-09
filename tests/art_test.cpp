/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0.
 * Derived from thirdparty/libart/tests/test_art.c which is
 * Copyright 2012 Armon Dadgar. See thirdparty/libart/LICENSE. */
#include "watchman_system.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <inttypes.h>

#include "thirdparty/libart/src/art.h"

// This has to come after the art.h include because the MSVC
// runtime gets confused by the #define fail in tap.h
#include "thirdparty/tap.h"

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
    t.insert(buf, line);
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

  t.insert(std::string(reinterpret_cast<char*>(key1), 299), (void*)key1);
  t.insert(std::string(reinterpret_cast<char*>(key2), 302), (void*)key2);
  t.insert(std::string(reinterpret_cast<char*>(key2), 302), (void*)key2);
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
    t.insert(std::string(buf), line);
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
      uintptr_t val = *t.search(buf);
      if (line != val) {
        fail("Line: %d Val: %" PRIuPTR " Str: %s", line, val, buf);
      }
    }
    line++;
  }

  // Check the minimum
  auto l = t.minimum();
  fail_unless(l && l->key == "A");

  // Check the maximum
  l = t.maximum();
  fail_unless(l && l->key == "zythum");
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
    t.insert(buf, line);
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
    val = *t.search(buf);
    if (line != val) {
      fail("Line: %d Val: %" PRIuPTR " Str: %s", line, val, buf);
    }

    // Delete, should get lineno back
    if (!t.erase(buf)) {
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

void test_art_insert_iter(void) {
  art_tree<uintptr_t> t;

  int len;
  char buf[512];
  FILE *f = open_test_file("thirdparty/libart/tests/words.txt");

  uint64_t xor_mask = 0;
  uintptr_t lineno = 1, nlines;
  while (fgets(buf, sizeof buf, f)) {
    len = (int)strlen(buf);
    buf[len - 1] = '\0';
    t.insert(buf, lineno);

    xor_mask ^= (lineno * (buf[0] + len - 1));
    lineno++;
  }
  nlines = lineno - 1;

  {
    uint64_t out[] = {0, 0};
    fail_unless(t.iter([&out](const std::string& key, uintptr_t& line) {
      uint64_t mask = (line * (key[0] + key.size()));
      out[0]++;
      out[1] ^= mask;
      return 0;

    }) == 0);

    fail_unless(out[0] == nlines);
    fail_unless(out[1] == xor_mask);
  }
}

template <typename T>
struct prefix_data {
  int count;
  int max_count;
  const char **expected;

  int operator()(const std::string& k, T&) {
    fail_unless(count < max_count);
    diag("Key: %s Expect: %s", k.c_str(), expected[count]);
    fail_unless(memcmp(k.data(), expected[count], k.size()) == 0);
    count++;
    return 0;
  }
};

void test_art_iter_prefix(void) {
  art_tree<void*> t;
  const char *expected2[] = {"abc.123.456", "api",         "api.foe.fum",
                             "api.foo",     "api.foo.bar", "api.foo.baz"};

  t.insert("api.foo.bar", nullptr);
  t.insert("api.foo.baz", nullptr);
  t.insert("api.foe.fum", nullptr);
  t.insert("abc.123.456", nullptr);
  t.insert("api.foo", nullptr);
  t.insert("api", nullptr);

  {
    // Iterate over api
    const char *expected[] = {"api", "api.foe.fum", "api.foo", "api.foo.bar",
                              "api.foo.baz"};
    prefix_data<void*> p = {0, 5, expected};
    fail_unless(!t.iterPrefix((unsigned char*)"api", 3, p));
    diag("Count: %d Max: %d", p.count, p.max_count);
    fail_unless(p.count == p.max_count);
  }

  {
    // Iterate over 'a'
    prefix_data<void*> p2 = {0, 6, expected2};
    fail_unless(!t.iterPrefix((unsigned char*)"a", 1, p2));
    fail_unless(p2.count == p2.max_count);
  }

  {
    // Check a failed iteration
    prefix_data<void*> p3 = {0, 0, nullptr};
    fail_unless(!t.iterPrefix((unsigned char*)"b", 1, p3));
    fail_unless(p3.count == 0);
  }

  {
    // Iterate over api.
    const char *expected4[] = {"api.foe.fum", "api.foo", "api.foo.bar",
                               "api.foo.baz"};
    prefix_data<void*> p4 = {0, 4, expected4};
    fail_unless(!t.iterPrefix((unsigned char*)"api.", 4, p4));
    diag("Count: %d Max: %d", p4.count, p4.max_count);
    fail_unless(p4.count == p4.max_count);
  }

  {
    // Iterate over api.foo.ba
    const char *expected5[] = {"api.foo.bar"};
    prefix_data<void*> p5 = {0, 1, expected5};
    fail_unless(!t.iterPrefix((unsigned char*)"api.foo.bar", 11, p5));
    diag("Count: %d Max: %d", p5.count, p5.max_count);
    fail_unless(p5.count == p5.max_count);
  }

  // Check a failed iteration on api.end
  {
    prefix_data<void*> p6 = {0, 0, nullptr};
    fail_unless(!t.iterPrefix((unsigned char*)"api.end", 7, p6));
    fail_unless(p6.count == 0);
  }

  // Iterate over empty prefix
  {
    prefix_data<void*> p7 = {0, 6, expected2};
    fail_unless(!t.iterPrefix((unsigned char*)"", 0, p7));
    fail_unless(p7.count == p7.max_count);
  }
}

void test_art_long_prefix(void) {
  art_tree<uintptr_t> t;

  t.insert("this:key:has:a:long:prefix:3", 3);
  t.insert("this:key:has:a:long:common:prefix:2", 2);
  t.insert("this:key:has:a:long:common:prefix:1", 1);

  // Search for the keys
  fail_unless(1 == *t.search("this:key:has:a:long:common:prefix:1"));
  fail_unless(2 == *t.search("this:key:has:a:long:common:prefix:2"));
  fail_unless(3 == *t.search("this:key:has:a:long:prefix:3"));

  {
    const char *expected[] = {
        "this:key:has:a:long:common:prefix:1",
        "this:key:has:a:long:common:prefix:2", "this:key:has:a:long:prefix:3",
    };
    prefix_data<uintptr_t> p = {0, 3, expected};
    fail_unless(!t.iterPrefix((unsigned char*)"this:key:has", 12, p));
    diag("Count: %d Max: %d", p.count, p.max_count);
    fail_unless(p.count == p.max_count);
  }
}

void test_art_prefix(void) {
  art_tree<void*> t;
  void *v;

  t.insert("food", (void*)"food");
  t.insert("foo", (void*)"foo");
  diag("size is now %d", t.size());
  fail_unless(t.size() == 2);
  fail_unless((v = *t.search("food")) != nullptr);
  diag("food lookup yields %s", v);
  fail_unless(v && strcmp((char*)v, "food") == 0);

  t.iter([](const std::string& key, void*& value) {
    diag(
        "iter leaf: key_len=%d %.*s value=%p",
        int(key.size()),
        int(key.size()),
        key.data(),
        value);
    return 0;
  });

  fail_unless((v = *t.search("foo")) != nullptr);
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
    t.insert(buf, line);
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

    val = *t.search(buf);
    if (line != val) {
      fail("Line: %d Val: %" PRIuPTR " Str: %s\n", line, val, buf);
    }
    line++;
  }

  // Check the minimum
  auto l = t.minimum();
  diag("minimum is %s", l->key.c_str());
  fail_unless(l && l->key == "00026bda-e0ea-4cda-8245-522764e9f325");

  // Check the maximum
  l = t.maximum();
  diag("maximum is %s", l->key.c_str());
  fail_unless(l && l->key == "ffffcb46-a92e-4822-82af-a7190f9c1ec5");
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
