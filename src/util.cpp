#include "mvdb.h"

#include <charconv>
#include <cmath>

namespace mvdb {

std::string to_lower(std::string s) {
  for (char &c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool iequals(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  }
  return true;
}

bool starts_with(std::string_view s, std::string_view p) {
  return s.size() >= p.size() && s.substr(0, p.size()) == p;
}

std::string trim(std::string_view s) {
  size_t b = 0, e = s.size();
  while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  return std::string(s.substr(b, e - b));
}

static u32 crc_table[256];
static bool crc_table_ready = false;

static void init_crc_table() {
  if (crc_table_ready) return;
  for (u32 i = 0; i < 256; ++i) {
    u32 c = i;
    for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    crc_table[i] = c;
  }
  crc_table_ready = true;
}

u32 crc32_update(u32 crc, const void *data, size_t len) {
  init_crc_table();
  const u8 *p = static_cast<const u8 *>(data);
  crc = ~crc;
  for (size_t i = 0; i < len; ++i) crc = crc_table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
  return ~crc;
}

u32 crc32(const void *data, size_t len, u32 seed) { return crc32_update(seed, data, len); }

bool utf8_valid(std::string_view s) {
  size_t i = 0;
  const size_t n = s.size();
  while (i < n) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
      ++i;
    } else if ((c & 0xE0) == 0xC0) {
      if (i + 1 >= n) return false;
      unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
      if ((c1 & 0xC0) != 0x80) return false;
      unsigned cp = ((c & 0x1F) << 6) | (c1 & 0x3F);
      if (cp < 0x80) return false;
      i += 2;
    } else if ((c & 0xF0) == 0xE0) {
      if (i + 2 >= n) return false;
      unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
      unsigned char c2 = static_cast<unsigned char>(s[i + 2]);
      if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80) return false;
      unsigned cp = ((c & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F);
      if (cp < 0x800) return false;
      if (cp >= 0xD800 && cp <= 0xDFFF) return false;
      i += 3;
    } else if ((c & 0xF8) == 0xF0) {
      if (i + 3 >= n) return false;
      unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
      unsigned char c2 = static_cast<unsigned char>(s[i + 2]);
      unsigned char c3 = static_cast<unsigned char>(s[i + 3]);
      if ((c1 & 0xC0) != 0x80 || (c2 & 0xC0) != 0x80 || (c3 & 0xC0) != 0x80) return false;
      unsigned cp = ((c & 0x07) << 18) | ((c1 & 0x3F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F);
      if (cp < 0x10000 || cp > 0x10FFFF) return false;
      i += 4;
    } else {
      return false;
    }
  }
  return true;
}

size_t utf8_codepoints(std::string_view s) {
  size_t n = 0;
  for (size_t i = 0; i < s.size();) {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) i += 1;
    else if ((c & 0xE0) == 0xC0) i += 2;
    else if ((c & 0xF0) == 0xE0) i += 3;
    else i += 4;
    ++n;
  }
  return n;
}

int utf8_compare(std::string_view a, std::string_view b) {
  // Valid UTF-8 byte order == code point order.
  if (a < b) return -1;
  if (a > b) return 1;
  return 0;
}

bool utf8_equal(std::string_view a, std::string_view b) { return a == b; }

bool parse_i64(std::string_view s, i64 &out) {
  if (s.empty()) return false;
  const char *b = s.data();
  const char *e = b + s.size();
  i64 v = 0;
  auto r = std::from_chars(b, e, v);
  if (r.ec != std::errc() || r.ptr != e) return false;
  out = v;
  return true;
}

bool parse_double(std::string_view s, double &out) {
  if (s.empty()) return false;
  std::string tmp(s);
  char *end = nullptr;
  errno = 0;
  double v = std::strtod(tmp.c_str(), &end);
  if (end != tmp.c_str() + tmp.size()) return false;
  if (errno == ERANGE || !std::isfinite(v)) return false;
  out = v;
  return true;
}

std::string format_i64(i64 v) { return std::to_string(v); }

static std::string trim_float_string(std::string s) {
  if (s.find('.') != std::string::npos && s.find('e') == std::string::npos &&
      s.find('E') == std::string::npos) {
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
  }
  if (s == "-0") s = "0";
  return s;
}

std::string format_double(double v) {
  if (!std::isfinite(v)) return "nan";
  char buf[64];
  auto r = std::to_chars(buf, buf + sizeof(buf), v);
  if (r.ec == std::errc()) return trim_float_string(std::string(buf, r.ptr));
  std::snprintf(buf, sizeof(buf), "%.17g", v);
  return std::string(buf);
}

std::string format_float(float v) {
  if (!std::isfinite(v)) return "nan";
  char buf[64];
  auto r = std::to_chars(buf, buf + sizeof(buf), v);
  if (r.ec == std::errc()) return trim_float_string(std::string(buf, r.ptr));
  std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v));
  return trim_float_string(std::string(buf));
}

std::string escape_field(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '\t': out += "\\t"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

std::string unescape_field(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      char c = s[++i];
      switch (c) {
        case 't': out.push_back('\t'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case '\\': out.push_back('\\'); break;
        default: out.push_back(c); break;
      }
    } else {
      out.push_back(s[i]);
    }
  }
  return out;
}

Value Value::make_int(i64 x) {
  Value v;
  v.type = ValueType::INT;
  v.i = x;
  return v;
}
Value Value::make_string(std::string x) {
  Value v;
  v.type = ValueType::STRING;
  v.s = std::move(x);
  return v;
}
Value Value::make_vector(std::vector<float> x) {
  Value v;
  v.type = ValueType::VECTOR;
  v.v = std::move(x);
  return v;
}
Value Value::make_double(double x) {
  Value v;
  v.type = ValueType::DOUBLE;
  v.d = x;
  return v;
}

bool value_less(const Value &a, const Value &b) {
  if (a.type != b.type) {
    // Numeric comparison between INT and DOUBLE.
    if ((a.type == ValueType::INT || a.type == ValueType::DOUBLE) &&
        (b.type == ValueType::INT || b.type == ValueType::DOUBLE)) {
      double x = a.type == ValueType::INT ? static_cast<double>(a.i) : a.d;
      double y = b.type == ValueType::INT ? static_cast<double>(b.i) : b.d;
      return x < y;
    }
    return static_cast<int>(a.type) < static_cast<int>(b.type);
  }
  switch (a.type) {
    case ValueType::INT: return a.i < b.i;
    case ValueType::DOUBLE: return a.d < b.d;
    case ValueType::STRING: return utf8_compare(a.s, b.s) < 0;
    case ValueType::VECTOR: {
      if (a.v.size() != b.v.size()) return a.v.size() < b.v.size();
      for (size_t i = 0; i < a.v.size(); ++i) {
        if (a.v[i] != b.v[i]) return a.v[i] < b.v[i];
      }
      return false;
    }
  }
  return false;
}

bool value_equal(const Value &a, const Value &b) {
  if (a.type == b.type) {
    switch (a.type) {
      case ValueType::INT: return a.i == b.i;
      case ValueType::DOUBLE: return a.d == b.d;
      case ValueType::STRING: return a.s == b.s;
      case ValueType::VECTOR: return a.v == b.v;
    }
  }
  if ((a.type == ValueType::INT || a.type == ValueType::DOUBLE) &&
      (b.type == ValueType::INT || b.type == ValueType::DOUBLE)) {
    double x = a.type == ValueType::INT ? static_cast<double>(a.i) : a.d;
    double y = b.type == ValueType::INT ? static_cast<double>(b.i) : b.d;
    return x == y;
  }
  return false;
}

std::string value_to_string(const Value &v) {
  switch (v.type) {
    case ValueType::INT: return format_i64(v.i);
    case ValueType::DOUBLE: return format_double(v.d);
    case ValueType::STRING: return v.s;
    case ValueType::VECTOR: {
      std::string out = "[";
      for (size_t i = 0; i < v.v.size(); ++i) {
        if (i) out.push_back(',');
        out += format_float(v.v[i]);
      }
      out.push_back(']');
      return out;
    }
  }
  return {};
}

int TableSchema::find_column(std::string_view n) const {
  for (size_t i = 0; i < columns.size(); ++i) {
    if (iequals(columns[i].name, n)) return static_cast<int>(i);
  }
  return -1;
}

int TableSchema::vector_index() const {
  for (size_t i = 0; i < columns.size(); ++i) {
    if (columns[i].type == ColType::VECTOR) return static_cast<int>(i);
  }
  return -1;
}

bool path_exists(const std::string &path) {
  struct stat st;
  return ::stat(path.c_str(), &st) == 0;
}

void make_dir(const std::string &path) {
  if (path.empty()) return;
  std::string cur;
  for (size_t i = 0; i < path.size(); ++i) {
    cur.push_back(path[i]);
    if (path[i] == '/' || i + 1 == path.size()) {
      if (cur == "/" || cur.empty()) continue;
      std::string p = cur;
      while (!p.empty() && p.back() == '/') p.pop_back();
      if (p.empty()) continue;
      if (::mkdir(p.c_str(), 0755) != 0 && errno != EEXIST) {
        fail(1105, "cannot create directory " + p + ": " + std::strerror(errno));
      }
    }
  }
}

void fsync_dir(const std::string &path) {
  std::string dir = path;
  while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
  struct stat st;
  if (::stat(dir.c_str(), &st) == 0 && !S_ISDIR(st.st_mode)) {
    size_t slash = dir.find_last_of('/');
    if (slash == std::string::npos) dir = ".";
    else if (slash == 0) dir = "/";
    else dir = dir.substr(0, slash);
  }
  int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd >= 0) {
    ::fsync(fd);
    ::close(fd);
  }
}

void write_all(int fd, const void *data, size_t len, off_t offset) {
  const char *p = static_cast<const char *>(data);
  size_t left = len;
  while (left > 0) {
    ssize_t n = ::pwrite(fd, p, left, offset);
    if (n < 0) {
      if (errno == EINTR) continue;
      fail(1105, std::string("write failed: ") + std::strerror(errno));
    }
    if (n == 0) fail(1105, "short write");
    p += n;
    left -= static_cast<size_t>(n);
    offset += n;
  }
}

void read_all(int fd, void *data, size_t len, off_t offset) {
  char *p = static_cast<char *>(data);
  size_t left = len;
  while (left > 0) {
    ssize_t n = ::pread(fd, p, left, offset);
    if (n < 0) {
      if (errno == EINTR) continue;
      fail(1105, std::string("read failed: ") + std::strerror(errno));
    }
    if (n == 0) fail(1105, "unexpected end of file");
    p += n;
    left -= static_cast<size_t>(n);
    offset += n;
  }
}

u64 file_size(int fd) {
  struct stat st;
  if (::fstat(fd, &st) != 0) return 0;
  return static_cast<u64>(st.st_size);
}

void atomic_write_file(const std::string &path, const std::string &content) {
  std::string tmp = path + ".tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) fail(1105, "cannot create " + tmp + ": " + std::strerror(errno));
  try {
    write_all(fd, content.data(), content.size(), 0);
    ::fsync(fd);
  } catch (...) {
    ::close(fd);
    ::unlink(tmp.c_str());
    throw;
  }
  ::close(fd);
  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    ::unlink(tmp.c_str());
    fail(1105, "cannot rename " + tmp + ": " + std::strerror(errno));
  }
  fsync_dir(path);
}

}  // namespace mvdb
