// Microbenchmark for the per-row string comparison in the filter path.
//
// The scan evaluates the WHERE clause once per row.  The current path
// materialises two Value objects (the column value is copied out of the row
// and the literal is copied out of the parsed expression) and then compares
// them.  The candidate path reads the column value in place and compares the
// bytes directly.  utf8_compare is already byte-wise for valid UTF-8, so the
// two paths are semantically identical.
//
// Build: g++ -O2 -std=c++17 tests/bench_string_compare.cpp src/util.cpp \
//          -o build/bench_string_compare
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "../src/mvdb.h"

using namespace mvdb;

int main(int argc, char **argv) {
  const int rows = argc > 1 ? atoi(argv[1]) : 10000;
  const int iters = argc > 2 ? atoi(argv[2]) : 200;

  std::vector<std::string> col;
  col.reserve(rows);
  for (int i = 0; i < rows; ++i) col.push_back("c" + std::to_string(i % 4));
  const std::string literal = "c0";

  long long hits_a = 0;
  auto t0 = std::chrono::steady_clock::now();
  for (int it = 0; it < iters; ++it) {
    for (int i = 0; i < rows; ++i) {
      // Current path: copy the column value into a Value, copy the literal
      // into a second Value, then compare.
      Value a;
      a.type = ValueType::STRING;
      a.s = col[i];
      Value b = Value::make_string(literal);
      if (utf8_compare(a.s, b.s) == 0) ++hits_a;
    }
  }
  auto t1 = std::chrono::steady_clock::now();

  long long hits_b = 0;
  for (int it = 0; it < iters; ++it) {
    for (int i = 0; i < rows; ++i) {
      // Candidate path: read the column value in place, compare bytes.
      if (utf8_equal(col[i], literal)) ++hits_b;
    }
  }
  auto t2 = std::chrono::steady_clock::now();

  double a_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  double b_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
  double a_ns = a_ms * 1e6 / (double)rows / (double)iters;
  double b_ns = b_ms * 1e6 / (double)rows / (double)iters;

  std::printf("rows=%d iters=%d\n", rows, iters);
  std::printf("current_path_ns_per_row=%.2f (hits=%lld)\n", a_ns, hits_a);
  std::printf("candidate_path_ns_per_row=%.2f (hits=%lld)\n", b_ns, hits_b);
  std::printf("saved_ratio=%.4f\n", a_ns > 0 ? (a_ns - b_ns) / a_ns : 0.0);
  std::printf("speedup=%.3f\n", b_ns > 0 ? a_ns / b_ns : 0.0);
  return (hits_a == hits_b) ? 0 : 1;
}
