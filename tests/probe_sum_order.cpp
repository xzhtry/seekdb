// Does the summation order change the answer of a tie?
//
// The student set the red line for the distance kernel (u33): changing the
// order of the floating point summation would break the rule "rows at the
// same distance are ordered by the primary key", which is what the admission
// case checks.  This program copies the implementations of src/engine.cpp
// verbatim and adds one plausible alternative kernel, then feeds all of them
// the same constructed data.
//
//   query  q = (0, 0, 0, 0, b, 0, 0, 0, c, 0, 0, 0, d, 0 .. 0)
//   row 1  r1 = (a, 0, 0, 0, b, 0, 0, 0, c, 0, 0, 0, d, 0 .. 0)
//   row 2  r2 = (d, 0, 0, 0, c, 0, 0, 0, b, 0, 0, 0, a, 0 .. 0)
//
// The non zero components sit at the indices 0, 4, 8 and 12, so the two rows
// carry the same multiset of squared differences and are equidistant in exact
// arithmetic.  The order of the four values is mirrored between the rows.
//
//   scalar (sum in index order)      (((a^2+b^2)+c^2)+d^2)   vs  (((d^2+c^2)+b^2)+a^2)
//   AVX2 (four double lanes + FMA)   (a^2+c^2) + (b^2+d^2)     vs  (b^2+d^2) + (a^2+c^2)
//
// The AVX2 tree adds the same two summands, so it ties exactly and the primary
// key decides.  The scalar tree re-associates the sum, so the two rows differ
// by about one ulp and the distance decides instead.
//
//   build: g++ -O2 -mavx2 -mfma -o sum_order probe_sum_order.cpp
#include <immintrin.h>

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <random>
#include <vector>

// --- copied verbatim from src/engine.cpp -------------------------------
static double l2_scalar(const float *a, const float *b, size_t n) {
  double sum = 0;
  for (size_t i = 0; i < n; ++i) {
    double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
    sum += d * d;
  }
  return std::sqrt(sum);
}

__attribute__((target("avx2,fma"))) static double l2_avx2(const float *a, const float *b,
                                                           size_t n) {
  __m256d sum0 = _mm256_setzero_pd();
  __m256d sum1 = _mm256_setzero_pd();
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256 va = _mm256_loadu_ps(a + i);
    __m256 vb = _mm256_loadu_ps(b + i);
    __m256 d = _mm256_sub_ps(va, vb);
    __m128 lo = _mm256_castps256_ps128(d);
    __m128 hi = _mm256_extractf128_ps(d, 1);
    __m256d dlo = _mm256_cvtps_pd(lo);
    __m256d dhi = _mm256_cvtps_pd(hi);
    sum0 = _mm256_fmadd_pd(dlo, dlo, sum0);
    sum1 = _mm256_fmadd_pd(dhi, dhi, sum1);
  }
  double lanes0[4], lanes1[4];
  _mm256_storeu_pd(lanes0, sum0);
  _mm256_storeu_pd(lanes1, sum1);
  double total = 0;
  for (double v : lanes0) total += v;
  for (double v : lanes1) total += v;
  for (; i < n; ++i) {
    double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
    total += d * d;
  }
  return std::sqrt(total);
}
// -----------------------------------------------------------------------

// A plausible alternative kernel: the same loop, but the product is fused
// into the accumulation.  It is one instruction different from l2_scalar.
__attribute__((target("avx2,fma"))) static double l2_scalar_fma(const float *a, const float *b,
                                                                 size_t n) {
  double sum = 0;
  for (size_t i = 0; i < n; ++i) {
    double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
    sum = std::fma(d, d, sum);
  }
  return std::sqrt(sum);
}

static const size_t kDims = 128;

struct Impl {
  const char *name;
  double (*fn)(const float *, const float *, size_t);
};

int main() {
  std::mt19937 rng(20261115);
  std::uniform_real_distribution<float> dist(0.0f, 1.0f);

  std::vector<float> q(kDims, 0.0f), r1(kDims, 0.0f), r2(kDims, 0.0f);
  const int idx[4] = {0, 4, 8, 12};

  Impl impls[3] = {
      {"scalar   (sum in index order)", l2_scalar},
      {"avx2     (4 double lanes + FMA, what the server runs)", l2_avx2},
      {"scalar+fma (one instruction different from scalar)", l2_scalar_fma},
  };

  long long tried = 0;
  bool found = false;
  float a = 0, b = 0, c = 0, d = 0;
  double vals[3][2];

  for (long long iter = 0; iter < 2000000 && !found; ++iter) {
    a = dist(rng); b = dist(rng); c = dist(rng); d = dist(rng);
    r1[idx[0]] = a; r1[idx[1]] = b; r1[idx[2]] = c; r1[idx[3]] = d;
    r2[idx[0]] = d; r2[idx[1]] = c; r2[idx[2]] = b; r2[idx[3]] = a;
    ++tried;

    bool tie_all = true;
    bool flipped = false;
    for (int k = 0; k < 3; ++k) {
      vals[k][0] = impls[k].fn(r1.data(), q.data(), kDims);
      vals[k][1] = impls[k].fn(r2.data(), q.data(), kDims);
      if (vals[k][0] != vals[k][1]) tie_all = false;
    }
    // The case we are after: the kernel the server runs ties exactly, so the
    // primary key decides and the smaller id wins, while an alternative kernel
    // says the other row is closer and therefore returns the other row first.
    if (vals[1][0] == vals[1][1]) {
      for (int k = 0; k < 3; ++k) {
        if (k == 1) continue;
        if (vals[k][0] != vals[k][1] && vals[k][1] < vals[k][0]) flipped = true;
      }
    }
    if (flipped) found = true;
  }

  printf("dims=%zu  tried=%lld\n", kDims, tried);
  if (!found) {
    printf("no case where the server kernel ties and an alternative kernel does not\n");
    return 1;
  }
  printf("a=%.9g b=%.9g c=%.9g d=%.9g\n", a, b, c, d);
  printf("the two rows carry the same squared differences in mirrored order\n\n");
  for (int k = 0; k < 3; ++k) {
    const char *winner;
    if (vals[k][0] == vals[k][1]) winner = "tie -> the smaller primary key wins (row 1)";
    else winner = (vals[k][0] < vals[k][1]) ? "row 1 is closer" : "row 2 is closer";
    printf("  %-50s d(row1)=%.17g d(row2)=%.17g  %s\n", impls[k].name, vals[k][0], vals[k][1],
           winner);
  }
  printf("\nulp difference between the two rows under the alternative kernels:\n");
  for (int k = 1; k < 3; ++k) {
    double delta = vals[k][0] - vals[k][1];
    double ulp = std::nextafter(std::fabs(vals[k][0]), INFINITY) - std::fabs(vals[k][0]);
    printf("  %-50s delta=%.3g (%.2f ulp)\n", impls[k].name, delta,
           ulp > 0 ? std::fabs(delta) / ulp : 0.0);
  }
  return 0;
}
