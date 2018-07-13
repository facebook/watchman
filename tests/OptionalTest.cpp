#include "watchman_system.h"
#include "Optional.h"
#include <string>
#include "thirdparty/tap.h"

using namespace watchman;

struct AdviseDestroy {
  bool* destroyed{nullptr};

  explicit AdviseDestroy(bool* destroyed) : destroyed(destroyed) {
    *destroyed = false;
  }

  ~AdviseDestroy() {
    if (destroyed) {
      if (*destroyed) {
        throw std::runtime_error("already marked destroyed!?");
      }
      *destroyed = true;
    }
  }

  AdviseDestroy(AdviseDestroy&& other) noexcept : destroyed(other.destroyed) {
    *this = std::move(other);
  }

  AdviseDestroy& operator=(AdviseDestroy&& other) {
    if (&other != this) {
      destroyed = other.destroyed;
      other.destroyed = nullptr;
    }
    return *this;
  }
};

void test_assign() {
  Optional<bool> b;
  ok(!b.has_value(), "default constructs empty");
  b.reset();
  ok(!b.has_value(), "still empty after reset");

  b = true;
  ok(b.has_value(), "assignment changes has_value state");
  ok(b.value(), "stored true");
  ok(*b, "derefs to true");

  b.value() = false;
  ok(!*b, "assigned to false");
}

void test_reset() {
  Optional<bool> b{false};
  ok(b.has_value(), "second assignment still has_value state");
  ok(!b.value(), "stored false");
  ok(!*b, "derefs to false");

  b.reset();
  ok(!b.has_value(), "still empty after reset");
}

void test_throw_on_empty() {
  Optional<bool> b;
  try {
    b.value();
    ok(false, "should not get here");
  } catch (const BadOptionalAccess&) {
    ok(true, "accessing value for empty optional throws error");
  }
}

void test_dtor() {
  bool a_destroyed = false;
  { Optional<AdviseDestroy> a{AdviseDestroy(&a_destroyed)}; }
  ok(a_destroyed, "destructor is run on destruction");

  {
    Optional<AdviseDestroy> a{AdviseDestroy(&a_destroyed)};
    Optional<AdviseDestroy> b;

    b = std::move(a);
    ok(!a_destroyed, "destructor is not run on move");
  }
  ok(a_destroyed, "destructor is run on destruction");

  bool b_destroyed = false;
  {
    Optional<AdviseDestroy> a{AdviseDestroy(&a_destroyed)};
    Optional<AdviseDestroy> b{AdviseDestroy(&b_destroyed)};

    b = std::move(a);
    ok(!a_destroyed, "destructor is not run on move");
    ok(b_destroyed, "b destructor was called when replaced");
  }
  ok(a_destroyed, "destructor is run on destruction");
}

struct Simple {
  int foo;
};

void test_operator() {
  Optional<Simple> s{Simple{1}};
  ok(s->foo == 1, "->op");
  s->foo = 2;
  ok(s->foo == 2, "->op reflects changed value");

  const Simple& s_ref_const = s.value();
  ok(s_ref_const.foo == 2, "const ref");

  Simple& s_ref = s.value();
  s_ref.foo = 3;
  ok(s->foo == 3, "mut ref updated value");
}

int main() {
  plan_tests(21);

  test_assign();
  test_reset();
  test_throw_on_empty();
  test_dtor();
  test_operator();

  return exit_status();
}
