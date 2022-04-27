/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <benchmark/benchmark.h>
#include "watchman/thirdparty/jansson/jansson.h"

namespace {
using namespace watchman;

void encode_doubles(benchmark::State& state) {
  // 3.7 ^ 500 still fits in a double.
  constexpr size_t N = 500;
  // Produce a variety of doubles.
  constexpr double B = 3.7;

  auto arr = json_array_of_size(N);
  double value = 1;
  for (size_t i = 0; i < N; ++i) {
    json_array_append(arr, json_real(value));
    value *= B;
  }

  for (auto _ : state) {
    benchmark::DoNotOptimize(json_dumps(arr, JSON_COMPACT));
  }
}
BENCHMARK(encode_doubles);

void encode_zero_point_zero(benchmark::State& state) {
  constexpr size_t N = 500;

  auto arr = json_array_of_size(N);
  for (size_t i = 0; i < N; ++i) {
    json_array_append(arr, json_real(0));
  }

  for (auto _ : state) {
    benchmark::DoNotOptimize(json_dumps(arr, JSON_COMPACT));
  }
}
BENCHMARK(encode_zero_point_zero);

} // namespace

BENCHMARK_MAIN();
