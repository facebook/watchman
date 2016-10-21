/* Copyright 2016-present Facebook, Inc.
 * Licensed under the Apache License, Version 2.0. */

#include "watchman.h"
#include "thirdparty/tap.h"

void test_ht_iter_del() {
  auto ht = w_ht_new(2, &w_ht_dict_funcs);
  for (int i = 0; i < 32; i++) {
    auto key = w_string::printf("key%d", i);
    auto val = w_string::printf("val%d", i);
    bool inserted = w_ht_set(ht, w_ht_ptr_val(key), w_ht_ptr_val(val));
    ok(inserted, "%s -> %s inserted", key.c_str(), val.c_str());
  }

  w_ht_iter_t citer;
  int count_iter = 0;
  if (w_ht_first(ht, &citer)) {
    do {
      bool deleted = w_ht_iter_del(ht, &citer);
      ok(deleted, "item %d deleted", count_iter);
      count_iter++;
    } while (w_ht_next(ht, &citer));
  }

  uint32_t final_size = w_ht_size(ht);
  ok(final_size == 0, "size after deletion: expected=0 actual=%d", final_size);

  ok(count_iter == 32,
     "count iterated over while deleting: expected=32 actual=%d", count_iter);
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  plan_tests(66);
  test_ht_iter_del();

  return exit_status();
}
