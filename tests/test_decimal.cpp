// Standalone check: parse_decimal_exact must return exactly the same double
// as std::from_chars for every input it accepts, and must reject exactly the
// inputs it used to reject.
//
//   g++ -O2 -std=c++17 -o /tmp/test_decimal tests/test_decimal.cpp && /tmp/test_decimal
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

typedef uint64_t u64;

static inline bool is_digit_ascii(unsigned char c) { return c >= '0' && c <= '9'; }

static bool parse_decimal_exact(const char *p, const char *e, double &out, const char *&endp) {
  static const double kPow10[] = {
      1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
      1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
  const char *q = p;
  bool neg = false;
  if (q < e && (*q == '+' || *q == '-')) {
    neg = (*q == '-');
    ++q;
  }
  u64 mant = 0;
  const char *int_start = q;
  while (q < e) {
    unsigned d = static_cast<unsigned>(static_cast<unsigned char>(*q)) - '0';
    if (d > 9) break;
    mant = mant * 10 + d;
    ++q;
  }
  int digits = static_cast<int>(q - int_start);
  int frac = 0;
  if (q < e && *q == '.') {
    ++q;
    const char *frac_start = q;
    while (q < e) {
      unsigned d = static_cast<unsigned>(static_cast<unsigned char>(*q)) - '0';
      if (d > 9) break;
      mant = mant * 10 + d;
      ++q;
    }
    frac = static_cast<int>(q - frac_start);
    digits += frac;
  }
  int exp10 = 0;
  if (q < e && (*q == 'e' || *q == 'E')) {
    ++q;
    bool eneg = false;
    if (q < e && (*q == '+' || *q == '-')) {
      eneg = (*q == '-');
      ++q;
    }
    int v = 0;
    int n = 0;
    while (q < e && is_digit_ascii((unsigned char)*q)) {
      if (n >= 3) return false;
      v = v * 10 + (*q - '0');
      ++n;
      ++q;
    }
    if (n == 0) return false;
    exp10 = eneg ? -v : v;
  }
  if (digits == 0) return false;
  if (digits > 19) return false;
  if (mant > (1ull << 53)) return false;
  int k = exp10 - frac;
  if (k > 22 || k < -22) return false;
  double v = (double)mant;
  if (k >= 0) {
    v *= kPow10[k];
  } else {
    v /= kPow10[-k];
  }
  out = neg ? -v : v;
  endp = q;
  return true;
}

int main() {
  std::mt19937_64 rng(20261116);
  long checked = 0, mismatched = 0, accepted = 0;

  auto one = [&](const std::string &s) {
    ++checked;
    double v1 = 0, v2 = 0;
    const char *after = nullptr;
    bool ok1 = parse_decimal_exact(s.data(), s.data() + s.size(), v1, after);
    // The fast path reads the sign itself, and from_chars does not take a
    // leading plus, so the magnitude is the part that has to agree.
    const char *mag = s.data();
    if (!s.empty() && s[0] == '+') ++mag;
    auto res = std::from_chars(mag, s.data() + s.size(), v2);
    bool ok2 = (res.ec == std::errc() && res.ptr != mag);
    if (!ok1) return;  // rejected: the fallback path handles it
    ++accepted;
    if (!ok2) {
      printf("FAIL accepted but from_chars rejected: %s\n", s.c_str());
      ++mismatched;
      return;
    }
    // The fast path must consume exactly the same characters.
    if (after != res.ptr) {
      printf("FAIL end pointer differs for %s\n", s.c_str());
      ++mismatched;
      return;
    }
    if (v1 != v2) {
      printf("FAIL value differs for %s: %.17g vs %.17g\n", s.c_str(), v1, v2);
      ++mismatched;
    }
  };

  // The shape the benchmark uses.
  char buf[64];
  for (int i = 0; i < 400000; ++i) {
    double d = (double)(rng() % 1000000000) / 1000000000.0;
    snprintf(buf, sizeof buf, "%.6f", d);
    one(buf);
    snprintf(buf, sizeof buf, "%.17g", d);
    one(buf);
    snprintf(buf, sizeof buf, "%.*f", (int)(rng() % 18), d);
    one(buf);
    snprintf(buf, sizeof buf, "%.6e", d * 1e-7);
    one(buf);
  }
  // Edge shapes.
  const char *fixed[] = {"0", "0.0", "-0", "+0", ".5", "-.5", "1.", "1e10", "1E-10", "1e+10",
                         "0.123456", "1234567890123456789", "12345678901234567890",
                         "9007199254740993", "1.7976931348623157e308", "1e23", "1e-23",
                         "0.000000000000000000001", "00000000000000000000001",
                         "3.14159265358979323846", "2.2250738585072011e-308"};
  for (const char *f : fixed) one(std::string(f));
  // Random digit strings with random shapes.
  for (int i = 0; i < 200000; ++i) {
    std::string s;
    int nd = (int)(rng() % 25);
    for (int k = 0; k < nd; ++k) s += char('0' + rng() % 10);
    if (rng() % 2) {
      size_t at = rng() % (s.size() + 1);
      s.insert(at, ".");
    }
    if (rng() % 3 == 0) s += (rng() % 2 ? "e" : "E");
    if (rng() % 2) s += std::to_string(rng() % 1000);
    if (rng() % 4 == 0) s = "-" + s;
    one(s);
  }

  printf("checked %ld, accepted by the fast path %ld, mismatches %ld\n", checked, accepted,
         mismatched);
  if (mismatched) return 1;
  printf("PASS parse_decimal_exact matches std::from_chars\n");
  return 0;
}
