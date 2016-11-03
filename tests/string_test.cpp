/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0. */

#include "watchman.h"
#include "watchman_string.h"
#include "thirdparty/tap.h"

void test_integrals() {
  ok(w_string::build(int8_t(1)) == w_string("1"), "made 1");
  ok(w_string::build(int16_t(1)) == w_string("1"), "made 1");
  ok(w_string::build(int32_t(1)) == w_string("1"), "made 1");
  ok(w_string::build(int64_t(1)) == w_string("1"), "made 1");

  ok(w_string::build(int8_t(-1)) == w_string("-1"), "made -1");
  ok(w_string::build(int16_t(-1)) == w_string("-1"), "made -1");
  ok(w_string::build(int32_t(-1)) == w_string("-1"), "made -1");
  ok(w_string::build(int64_t(-1)) == w_string("-1"), "made -1");

  ok(w_string::build(uint8_t(1)) == w_string("1"), "made 1");
  ok(w_string::build(uint16_t(1)) == w_string("1"), "made 1");
  ok(w_string::build(uint32_t(1)) == w_string("1"), "made 1");
  ok(w_string::build(uint64_t(1)) == w_string("1"), "made 1");

  ok(w_string::build(uint8_t(255)) == w_string("255"), "made 255");
  ok(w_string::build(uint16_t(255)) == w_string("255"), "made 255");
  ok(w_string::build(uint32_t(255)) == w_string("255"), "made 255");
  ok(w_string::build(uint64_t(255)) == w_string("255"), "made 255");

  ok(w_string::build(int8_t(-127)) == w_string("-127"), "made -127");

  ok(w_string::build(bool(true)) == w_string("1"), "true -> 1");
  ok(w_string::build(bool(false)) == w_string("0"), "false -> 0");
}

void test_strings() {
  {
    auto hello = w_string::build("hello");
    ok(hello == w_string("hello"), "hello");
    ok(hello.size() == 5, "there are 5 chars in hello");
    ok(!strcmp("hello", hello.c_str()),
       "looks nul terminated `%s` %" PRIu32,
       hello.c_str(),
       strlen_uint32(hello.c_str()));
  }

  {
    w_string_piece piece("hello");
    ok(piece.size() == 5, "piece has 5 char size");
    auto hello = w_string::build(piece);
    ok(hello.size() == 5, "hello has 5 char size");
    ok(!strcmp("hello", hello.c_str()), "looks nul terminated");
  }

  {
    char foo[] = "foo";
    auto str = w_string::build(foo);
    ok(str.size() == 3, "foo has 3 char size");
    ok(!strcmp("foo", foo), "foo matches");
  }
}

void test_pointers() {
  bool foo = true;
  char lowerBuf[20];

  auto str = w_string::build(&foo);
  snprintf(
      lowerBuf, sizeof(lowerBuf), "0x%" PRIx64, (uint64_t)(uintptr_t)(&foo));
  ok(str.size() == strlen_uint32(lowerBuf),
     "reasonable seeming bool pointer len, got %" PRIu32
     " vs expected %" PRIu32,
     str.size(),
     strlen_uint32(lowerBuf));
  ok(str.size() == strlen_uint32(str.c_str()),
     "string is really nul terminated, size %" PRIu32
     " strlen of c_str %" PRIu32,
     str.size(),
     strlen_uint32(str.c_str()));
  ok(!strcmp(lowerBuf, str.c_str()),
     "bool pointer rendered right hex value sprintf->%s, str->%s",
     lowerBuf,
     str.c_str());

  str = w_string::build(nullptr);
  ok(str.size() > 0, "nullptr has reasonable size: %" PRIsize_t, str.size());
  ok(str == w_string("0x0"), "nullptr looks right %s", str.c_str());

  void* zero = 0;
  ok(w_string::build(zero) == "0x0", "zero pointer looks right");
}

void test_double() {
  auto str = w_string::build(5.5);
  char buf[16];
  snprintf(buf, sizeof(buf), "%f", 5.5);
  ok(str.size() == 8, "size is %" PRIsize_t, str.size());
  ok(!strcmp(str.c_str(), buf), "str.c_str=%s, buf=%s", str.c_str(), buf);
  ok(str == w_string("5.500000"), "double looks good '%s'", str.c_str());
}

void test_concat() {
  auto str = w_string::build("one", 2, "three", 1.2, false);
  ok(str == w_string("one2three1.2000000"), "concatenated to %s", str.c_str());
}

int main(int, char**) {
  plan_tests(37);
  test_integrals();
  test_strings();
  test_pointers();
  test_double();
  test_concat();

  return exit_status();
}
