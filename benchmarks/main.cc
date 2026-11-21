// SPDX-License-Identifier: Apache-2.0
// Copyright The Umbra Authors
//
// Entry point of every benchmark executable (see umbra_add_benchmark).

#include <benchmark/benchmark.h>

#include "umbra/core/floating_point.h"

int main(int argc, char** argv) {
  umbra::core::ConfigureThreadFloatingPoint();
  benchmark::Initialize(&argc, argv);
  if (benchmark::ReportUnrecognizedArguments(argc, argv)) {
    return 1;
  }
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
  return 0;
}
