// Standalone check: the slice by eight CRC has to return exactly the same
// value as the byte at a time table lookup for every input and every length.
//
//   g++ -O2 -std=c++17 -o /tmp/test_crc32 tests/test_crc32.cpp && /tmp/test_crc32
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

typedef uint32_t u32;
typedef uint8_t u8;

static u32 ref_table[256];
static u32 slice_table[8][256];

static void init_ref() {
  for (u32 i = 0; i < 256; ++i) {
    u32 c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    ref_table[i] = c;
  }
}

static void init_slice() {
  for (u32 i = 0; i < 256; ++i) {
    u32 c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    slice_table[0][i] = c;
  }
  for (u32 i = 0; i < 256; ++i) {
    u32 c = slice_table[0][i];
    for (int k = 1; k < 8; ++k) {
      c = slice_table[0][c & 0xff] ^ (c >> 8);
      slice_table[k][i] = c;
    }
  }
}

// The implementation that was in util.cpp before the change.
static u32 crc_ref(u32 crc, const void *data, size_t len) {
  const u8 *p = static_cast<const u8 *>(data);
  crc = ~crc;
  for (size_t i = 0; i < len; ++i) crc = ref_table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
  return ~crc;
}

// The implementation that is in util.cpp now.
static u32 crc_slice(u32 crc, const void *data, size_t len) {
  const u8 *p = static_cast<const u8 *>(data);
  crc = ~crc;
  while (len >= 8) {
    u32 c = crc ^ (static_cast<u32>(p[0]) | (static_cast<u32>(p[1]) << 8) |
                   (static_cast<u32>(p[2]) << 16) | (static_cast<u32>(p[3]) << 24));
    crc = slice_table[7][c & 0xff] ^ slice_table[6][(c >> 8) & 0xff] ^
          slice_table[5][(c >> 16) & 0xff] ^ slice_table[4][(c >> 24) & 0xff] ^
          slice_table[3][p[4]] ^ slice_table[2][p[5]] ^ slice_table[1][p[6]] ^
          slice_table[0][p[7]];
    p += 8;
    len -= 8;
  }
  for (size_t i = 0; i < len; ++i) crc = slice_table[0][(crc ^ p[i]) & 0xff] ^ (crc >> 8);
  return ~crc;
}

int main() {
  init_ref();
  init_slice();

  // The check value of the reflected IEEE polynomial.
  const char *check = "123456789";
  if (crc_ref(0, check, 9) != 0xCBF43926u || crc_slice(0, check, 9) != 0xCBF43926u) {
    printf("FAIL check value: ref=%08x slice=%08x\n", crc_ref(0, check, 9),
           crc_slice(0, check, 9));
    return 1;
  }

  std::mt19937 rng(20261116);
  std::vector<u8> buf(1 << 16);
  for (size_t i = 0; i < buf.size(); ++i) buf[i] = static_cast<u8>(rng());

  // Every length from 0 to 600 and a few large ones, with several seeds.
  for (size_t len = 0; len <= 600; ++len) {
    for (u32 seed : {0u, 1u, 0xFFFFFFFFu, 0x12345678u, 0x9E3779B9u}) {
      u32 a = crc_ref(seed, buf.data(), len);
      u32 b = crc_slice(seed, buf.data(), len);
      if (a != b) {
        printf("FAIL len=%zu seed=%08x ref=%08x slice=%08x\n", len, seed, a, b);
        return 1;
      }
    }
  }
  for (size_t len : {1024u, 4096u, 65536u, 65535u, 65537u}) {
    for (u32 seed : {0u, 0xFFFFFFFFu}) {
      u32 a = crc_ref(seed, buf.data(), len);
      u32 b = crc_slice(seed, buf.data(), len);
      if (a != b) {
        printf("FAIL len=%zu seed=%08x ref=%08x slice=%08x\n", len, seed, a, b);
        return 1;
      }
    }
  }

  // Chained updates have to stay in step as well.
  u32 a = 0, b = 0;
  size_t off = 0;
  while (off < buf.size()) {
    size_t n = 1 + (rng() % 977);
    if (off + n > buf.size()) n = buf.size() - off;
    a = crc_ref(a, buf.data() + off, n);
    b = crc_slice(b, buf.data() + off, n);
    off += n;
  }
  if (a != b) {
    printf("FAIL chained: ref=%08x slice=%08x\n", a, b);
    return 1;
  }

  printf("PASS crc32 slice-by-8 matches the byte at a time table\n");
  return 0;
}
