#pragma once

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

namespace mvdb {

using i64 = int64_t;
using u64 = uint64_t;
using u32 = uint32_t;
using u8 = uint8_t;

// ---------------------------------------------------------------------------
// Errors.  Codes follow MySQL where a natural MySQL error exists.  The exact
// wording is what the test package compares, so keep messages stable.
// ---------------------------------------------------------------------------
struct Error {
  int code;
  std::string message;
};

[[noreturn]] inline void fail(int code, std::string message) {
  throw Error{code, std::move(message)};
}

constexpr int ER_SYNTAX = 1064;
constexpr int ER_TABLE_EXISTS = 1050;
constexpr int ER_BAD_TABLE = 1051;
constexpr int ER_UNKNOWN_TABLE = 1146;
constexpr int ER_BAD_FIELD = 1054;
constexpr int ER_DUP_ENTRY = 1062;
constexpr int ER_PARSE = 1064;
constexpr int ER_WRONG_VALUE_COUNT = 1136;
constexpr int ER_TOO_LONG = 1406;
constexpr int ER_OUT_OF_RANGE = 1264;
constexpr int ER_TRUNCATED_WRONG_VALUE = 1265;
constexpr int ER_UNSUPPORTED = 1235;
constexpr int ER_NO_SUCH_TXN = 1568;
constexpr int ER_TXN_ACTIVE = 1568;
constexpr int ER_BAD_NULL = 1048;
constexpr int ER_KEY_NOT_FOUND = 1176;
constexpr int ER_INVALID_VECTOR = 1366;
constexpr int ER_DATA_OUT_OF_RANGE = 1690;

// ---------------------------------------------------------------------------
// Small helpers.
// ---------------------------------------------------------------------------
std::string to_lower(std::string s);
bool iequals(std::string_view a, std::string_view b);
bool starts_with(std::string_view s, std::string_view p);
std::string trim(std::string_view s);

u32 crc32(const void *data, size_t len, u32 seed = 0);
u32 crc32_update(u32 crc, const void *data, size_t len);

// UTF-8 validation / code point count.  Returns false for malformed input.
bool utf8_valid(std::string_view s);
size_t utf8_codepoints(std::string_view s);
// Compare by Unicode code point.  For valid UTF-8, byte-wise comparison is
// equivalent to code point order.
int utf8_compare(std::string_view a, std::string_view b);
bool utf8_equal(std::string_view a, std::string_view b);

// Numbers ----------------------------------------------------------------
bool parse_i64(std::string_view s, i64 &out);
bool parse_double(std::string_view s, double &out);
// Canonical textual forms used in result sets.
std::string format_i64(i64 v);
std::string format_double(double v);
std::string format_float(float v);

// Escape/unescape tab separated protocol fields.
std::string escape_field(std::string_view s);
std::string unescape_field(std::string_view s);

// ---------------------------------------------------------------------------
// Values and rows.
// ---------------------------------------------------------------------------
enum class ValueType : u8 { INT = 1, STRING = 2, VECTOR = 3, DOUBLE = 4 };

struct Value {
  ValueType type = ValueType::INT;
  i64 i = 0;
  double d = 0;
  std::string s;
  std::vector<float> v;

  static Value make_int(i64 x);
  static Value make_string(std::string x);
  static Value make_vector(std::vector<float> x);
  static Value make_double(double x);
};

bool value_less(const Value &a, const Value &b);  // for ORDER BY
bool value_equal(const Value &a, const Value &b);
std::string value_to_string(const Value &v);

enum class ColType : u8 { BIGINT = 1, VARCHAR = 2, VECTOR = 3 };

struct Column {
  std::string name;
  ColType type = ColType::BIGINT;
  u32 param = 0;  // VARCHAR max code points / VECTOR dimension
};

struct TableSchema {
  u32 id = 0;
  std::string name;
  std::vector<Column> columns;
  int pk_index = -1;
  std::vector<std::pair<std::string, std::string>> vector_indexes;  // name -> column

  int find_column(std::string_view n) const;
  int vector_index() const;
};

struct Row {
  std::vector<Value> values;
};

// ---------------------------------------------------------------------------
// Little-endian binary helpers.
// ---------------------------------------------------------------------------
inline void put_u32(std::string &out, u32 v) {
  out.push_back(static_cast<char>(v & 0xff));
  out.push_back(static_cast<char>((v >> 8) & 0xff));
  out.push_back(static_cast<char>((v >> 16) & 0xff));
  out.push_back(static_cast<char>((v >> 24) & 0xff));
}
inline void put_u64(std::string &out, u64 v) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}
inline void put_i64(std::string &out, i64 v) { put_u64(out, static_cast<u64>(v)); }
inline u32 get_u32(const char *p) {
  return static_cast<u32>(static_cast<unsigned char>(p[0])) |
         (static_cast<u32>(static_cast<unsigned char>(p[1])) << 8) |
         (static_cast<u32>(static_cast<unsigned char>(p[2])) << 16) |
         (static_cast<u32>(static_cast<unsigned char>(p[3])) << 24);
}
inline u64 get_u64(const char *p) {
  u64 v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | static_cast<unsigned char>(p[i]);
  return v;
}
inline i64 get_i64(const char *p) { return static_cast<i64>(get_u64(p)); }

// ---------------------------------------------------------------------------
// File helpers.
// ---------------------------------------------------------------------------
bool path_exists(const std::string &path);
void make_dir(const std::string &path);
void fsync_dir(const std::string &path);
void write_all(int fd, const void *data, size_t len, off_t offset);
void read_all(int fd, void *data, size_t len, off_t offset);
u64 file_size(int fd);
void atomic_write_file(const std::string &path, const std::string &content);

// Runtime limits.
constexpr size_t kMaxRecordPayload = 64u * 1024u * 1024u;
constexpr size_t kDefaultCacheBudget = 64ull * 1024ull * 1024ull;
constexpr u32 kDataMagic = 0x5244564Du;   // "MVDR" little endian
constexpr u32 kWalMagic = 0x5744564Du;    // "MVDW"
constexpr u32 kCatalogMagic = 0x5441434Du;  // "MDAT" -> catalog tag
constexpr u32 kMetaMagic = 0x4154454Du;   // "META"

}  // namespace mvdb
