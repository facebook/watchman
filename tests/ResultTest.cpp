/* Copyright 2017-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0 */
#include "watchman_system.h"
#include <string>
#include "Result.h"
#include "thirdparty/tap.h"

using namespace watchman;

void test_empty() {
  Result<bool> b;

  ok(b.empty(), "default constructed and empty");

  try {
    b.throwIfError();
    ok(false, "expected to throw");
  } catch (const std::logic_error&) {
    ok(true, "throwIfError throws logic error for empty result");
  }

  try {
    b.value();
    ok(false, "expected to throw");
  } catch (const std::logic_error&) {
    ok(true, "throwIfError throws logic error for empty result");
  }

  try {
    b.error();
    ok(false, "expected to throw");
  } catch (const std::logic_error&) {
    ok(true, "throwIfError throws logic error for empty result");
  }
}

void test_simple_value() {
  auto b = makeResult(true);

  ok(!b.empty(), "b is not empty");
  ok(b.hasValue(), "b has a value");
  ok(b.value(), "b holds true");

  Result<bool> copyOfB(b);

  ok(!b.empty(), "b is not empty after being copied");
  ok(!copyOfB.empty(), "copyOfB is not empty");
  ok(copyOfB.hasValue(), "copyOfB has a value");
  ok(copyOfB.value(), "copyOfB holds true");

  Result<bool> movedB(std::move(b));

  ok(b.empty(), "b empty after move");
  ok(!movedB.empty(), "movedB is not empty");
  ok(movedB.hasValue(), "movedB has a value");
  ok(movedB.value(), "movedB holds true");

  b = movedB;
  ok(!b.empty(), "b is not empty after copying");
  ok(b.hasValue(), "b has a value");
  ok(b.value(), "b holds true");

  b = std::move(copyOfB);
  ok(!b.empty(), "b is not empty after copying");
  ok(b.hasValue(), "b has a value");
  ok(b.value(), "b holds true");
  ok(copyOfB.empty(), "copyOfB is empty after being moved");
}

void test_error() {
  auto a = makeResultWith([] { return std::string("noice"); });
  ok(a.hasValue(), "got a value");
  ok(a.value() == "noice", "got our string out");
  using atype = decltype(a);
  auto is_string = std::is_same<typename atype::value_type, std::string>::value;
  ok(is_string, "a has std::string as a value type");

  auto b = makeResultWith([] { throw std::runtime_error("w00t"); });

  ok(b.hasError(), "we got an exception contained");

  try {
    b.throwIfError();
    ok(false, "should not get here");
  } catch (const std::logic_error&) {
    ok(false, "should not have caught logic_error");
  } catch (const std::runtime_error& exc) {
    ok(!strcmp(exc.what(), "w00t"), "have our exception message in the error");
  }

  using btype = decltype(b);
  auto is_unit = std::is_same<typename btype::value_type, Unit>::value;
  ok(is_unit, "b has Unit as a value type");

  auto c = makeResultWith([] {
    if (false) {
      return 42;
    }
    throw std::runtime_error("gah");
  });

  using ctype = decltype(c);
  auto is_int = std::is_same<typename ctype::value_type, int>::value;
  ok(is_int, "c has int as a value type");

  ok(c.hasError(), "c has an error");
}

void test_non_exception_error_type() {
  Result<std::string, int> result("hello");

  ok(result.hasValue(), "has value");
  ok(result.value() == "hello", "has hello string");

  result = Result<std::string, int>(42);
  ok(result.hasError(), "holding error");
  ok(result.error() == 42, "holding 42");

  try {
    result.throwIfError();
    ok(false, "should not get here");
  } catch (const std::logic_error&) {
    ok(true, "got logic error");
  }
}

int main() {
  plan_tests(35);
  test_empty();
  test_simple_value();
  test_error();
  test_non_exception_error_type();
  return exit_status();
}
