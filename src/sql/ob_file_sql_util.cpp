/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define USING_LOG_PREFIX SQL
#include "sql/ob_file_sql_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <errno.h>
#ifndef _WIN32
#include <unistd.h>
#include <dirent.h>
#endif

#include "share/ob_errno.h"

using namespace oceanbase::common;

namespace oceanbase
{
namespace sql
{
namespace filesql
{

namespace
{

const int64_t FSQ_READ_BUF = 64 * 1024;
const int64_t FSQ_MAX_PATH = 4096;

inline bool is_space_char(const char c)
{
  return ' ' == c || '\t' == c || '\r' == c || '\n' == c || '\f' == c || '\v' == c;
}
inline bool is_digit_char(const char c) { return c >= '0' && c <= '9'; }
inline char lower_char(const char c)
{
  return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}
inline bool str_case_eq(const char *a, const int64_t alen, const char *b)
{
  const int64_t blen = static_cast<int64_t>(strlen(b));
  bool eq = (alen == blen);
  for (int64_t i = 0; eq && i < alen; ++i) {
    eq = (lower_char(a[i]) == lower_char(b[i]));
  }
  return eq;
}

int copy_string(ObIAllocator &alloc, const char *ptr, const int64_t len, ObString &out)
{
  int ret = OB_SUCCESS;
  char *buf = nullptr;
  if (len < 0) {
    ret = OB_INVALID_ARGUMENT;
  } else if (0 == len) {
    out.reset();
  } else if (OB_ISNULL(buf = static_cast<char *>(alloc.alloc(len)))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    MEMCPY(buf, ptr, len);
    out.assign_ptr(buf, static_cast<int32_t>(len));
  }
  return ret;
}

// ---------------------------------------------------------------- type guess

enum GuessKind
{
  GK_UNKNOWN = 0,
  GK_INT,
  GK_DOUBLE,
  GK_BOOL,
  GK_DATE,
  GK_DATETIME,
  GK_VARCHAR,
};

bool looks_like_int(const char *p, const int64_t len)
{
  bool res = false;
  if (len > 0 && len <= 20) {
    int64_t i = 0;
    if ('+' == p[0] || '-' == p[0]) {
      i = 1;
    }
    if (i < len) {
      res = true;
      for (; i < len; ++i) {
        if (!is_digit_char(p[i])) {
          res = false;
          break;
        }
      }
    }
  }
  return res;
}

bool looks_like_double(const char *p, const int64_t len)
{
  bool res = false;
  if (len > 0 && len <= 64) {
    int64_t i = 0;
    int64_t digits = 0;
    int64_t dots = 0;
    int64_t exps = 0;
    if ('+' == p[0] || '-' == p[0]) {
      i = 1;
    }
    const int64_t start = i;
    for (; i < len; ++i) {
      const char c = p[i];
      if (is_digit_char(c)) {
        ++digits;
      } else if ('.' == c) {
        ++dots;
      } else if ('e' == c || 'E' == c) {
        ++exps;
        if (i + 1 < len && ('+' == p[i + 1] || '-' == p[i + 1])) {
          ++i;
        }
        if (i + 1 >= len) {
          return false;
        }
      } else {
        return false;
      }
    }
    res = (i > start) && digits > 0 && dots <= 1 && exps <= 1;
  }
  return res;
}

bool looks_like_date(const char *p, const int64_t len)
{
  bool res = false;
  if (10 == len && '-' == p[4] && '-' == p[7]) {
    res = true;
    for (int64_t i = 0; res && i < len; ++i) {
      if (4 == i || 7 == i) {
        continue;
      }
      res = is_digit_char(p[i]);
    }
    if (res) {
      const int year = atoi(p);
      const int month = atoi(p + 5);
      const int day = atoi(p + 8);
      res = year >= 1 && year <= 9999 && month >= 1 && month <= 12 && day >= 1 && day <= 31;
    }
  }
  return res;
}

bool looks_like_datetime(const char *p, const int64_t len)
{
  bool res = false;
  if (len >= 19 && (' ' == p[10] || 'T' == p[10] || 't' == p[10])
      && ':' == p[13] && ':' == p[16]) {
    res = looks_like_date(p, 10);
    for (int64_t i = 11; res && i < 19; ++i) {
      if (13 == i || 16 == i) {
        continue;
      }
      res = is_digit_char(p[i]);
    }
    if (res) {
      const int hour = atoi(p + 11);
      const int minute = atoi(p + 14);
      const int second = atoi(p + 17);
      res = hour <= 23 && minute <= 59 && second <= 60;
    }
    if (res && len > 19) {
      if ('.' == p[19]) {
        for (int64_t i = 20; res && i < len; ++i) {
          res = is_digit_char(p[i]);
        }
      } else {
        res = false;
      }
    }
  }
  return res;
}

GuessKind guess_text_type(const char *p, const int64_t len)
{
  GuessKind kind = GK_VARCHAR;
  if (0 == len) {
    kind = GK_UNKNOWN;
  } else if (looks_like_int(p, len)) {
    kind = GK_INT;
  } else if (looks_like_double(p, len)) {
    kind = GK_DOUBLE;
  } else if (str_case_eq(p, len, "true") || str_case_eq(p, len, "false")) {
    kind = GK_BOOL;
  } else if (looks_like_date(p, len)) {
    kind = GK_DATE;
  } else if (looks_like_datetime(p, len)) {
    kind = GK_DATETIME;
  }
  return kind;
}

GuessKind merge_guess(const GuessKind a, const GuessKind b)
{
  GuessKind res = GK_VARCHAR;
  if (GK_UNKNOWN == a) {
    res = b;
  } else if (GK_UNKNOWN == b) {
    res = a;
  } else if (a == b) {
    res = a;
  } else if ((GK_INT == a || GK_DOUBLE == a) && (GK_INT == b || GK_DOUBLE == b)) {
    res = GK_DOUBLE;
  } else if ((GK_DATE == a && GK_DATETIME == b) || (GK_DATETIME == a && GK_DATE == b)) {
    res = GK_DATETIME;
  } else {
    res = GK_VARCHAR;
  }
  return res;
}

ObObjType guess_to_obj_type(const GuessKind kind, bool &is_bool)
{
  ObObjType type = ObVarcharType;
  is_bool = false;
  switch (kind) {
    case GK_INT: type = ObIntType; break;
    case GK_DOUBLE: type = ObDoubleType; break;
    case GK_BOOL: type = ObTinyIntType; is_bool = true; break;
    case GK_DATE: type = ObDateType; break;
    case GK_DATETIME: type = ObDateTimeType; break;
    default: type = ObVarcharType; break;
  }
  return type;
}

// ---------------------------------------------------------- date/datetime

int64_t days_from_civil(int y, int m, int d)
{
  y -= (m <= 2) ? 1 : 0;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

int parse_date_value(const char *p, const int64_t len, int32_t &value)
{
  int ret = OB_SUCCESS;
  if (len < 10) {
    ret = OB_INVALID_DATE_VALUE;
  } else {
    const int y = atoi(p);
    const int m = atoi(p + 5);
    const int d = atoi(p + 8);
    if (y < 1 || m < 1 || m > 12 || d < 1 || d > 31) {
      ret = OB_INVALID_DATE_VALUE;
    } else {
      value = static_cast<int32_t>(days_from_civil(y, m, d));
    }
  }
  return ret;
}

int parse_datetime_value(const char *p, const int64_t len, int64_t &value)
{
  int ret = OB_SUCCESS;
  int32_t date = 0;
  if (len < 19) {
    ret = OB_INVALID_DATE_VALUE;
  } else if (OB_FAIL(parse_date_value(p, 10, date))) {
  } else {
    const int64_t h = atoi(p + 11);
    const int64_t mi = atoi(p + 14);
    const int64_t s = atoi(p + 17);
    int64_t frac = 0;
    int64_t frac_digits = 0;
    if (len > 20 && '.' == p[19]) {
      for (int64_t i = 20; i < len && is_digit_char(p[i]) && frac_digits < 6; ++i) {
        frac = frac * 10 + (p[i] - '0');
        ++frac_digits;
      }
      for (int64_t i = frac_digits; i < 6; ++i) {
        frac *= 10;
      }
    }
    if (h > 23 || mi > 59 || s > 60) {
      ret = OB_INVALID_DATE_VALUE;
    } else {
      value = static_cast<int64_t>(date) * 86400000000LL
              + (h * 3600 + mi * 60 + s) * 1000000LL + frac;
    }
  }
  return ret;
}

int parse_text_cell(const char *p, const int64_t len, const FileSqlColumn &col, FileSqlValue &val)
{
  int ret = OB_SUCCESS;
  val.is_null_ = false;
  switch (col.type_) {
    case ObIntType:
    case ObTinyIntType:
    case ObSmallIntType:
    case ObMediumIntType:
    case ObInt32Type:
    case ObUInt64Type: {
      if (ObTinyIntType == col.type_
          && (str_case_eq(p, len, "true") || str_case_eq(p, len, "false"))) {
        val.int_val_ = (lower_char(p[0]) == 't') ? 1 : 0;
      } else {
        char buf[32];
        if (len <= 0 || len >= static_cast<int64_t>(sizeof(buf))) {
          ret = OB_INVALID_NUMERIC;
        } else {
          MEMCPY(buf, p, len);
          buf[len] = '\0';
          char *end = nullptr;
          errno = 0;
          const long long v = strtoll(buf, &end, 10);
          if (0 != errno || end == buf) {
            ret = OB_INVALID_NUMERIC;
          } else {
            val.int_val_ = static_cast<int64_t>(v);
          }
        }
      }
      break;
    }
    case ObFloatType:
    case ObDoubleType: {
      char buf[80];
      if (len <= 0 || len >= static_cast<int64_t>(sizeof(buf))) {
        ret = OB_INVALID_NUMERIC;
      } else {
        MEMCPY(buf, p, len);
        buf[len] = '\0';
        char *end = nullptr;
        val.dbl_val_ = strtod(buf, &end);
        if (end == buf) {
          ret = OB_INVALID_NUMERIC;
        }
      }
      break;
    }
    case ObDateType: {
      ret = parse_date_value(p, len, reinterpret_cast<int32_t &>(val.int_val_));
      break;
    }
    case ObDateTimeType:
    case ObTimestampType: {
      ret = parse_datetime_value(p, len, val.int_val_);
      break;
    }
    default: {
      val.str_val_.assign_ptr(p, static_cast<int32_t>(len));
      break;
    }
  }
  return ret;
}

FileSqlColumn *find_column(ObIArray<FileSqlColumn> &columns, const char *name, const int64_t len)
{
  FileSqlColumn *res = nullptr;
  for (int64_t i = 0; i < columns.count() && nullptr == res; ++i) {
    if (columns.at(i).name_.length() == len
        && 0 == strncasecmp(columns.at(i).name_.ptr(), name, static_cast<size_t>(len))) {
      res = &columns.at(i);
    }
  }
  return res;
}

// -------------------------------------------------------------- file access

int read_entire_file(const ObString &path, ObIAllocator &alloc, char *&data, int64_t &len)
{
  int ret = OB_SUCCESS;
  FILE *fp = nullptr;
  char buf[FSQ_MAX_PATH];
  data = nullptr;
  len = 0;
  if (path.length() <= 0 || path.length() >= static_cast<int32_t>(sizeof(buf))) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    MEMCPY(buf, path.ptr(), path.length());
    buf[path.length()] = '\0';
    if (nullptr == (fp = fopen(buf, "rb"))) {
      ret = OB_FILE_NOT_EXIST;
      LOG_WARN("failed to open file", K(ret), K(path));
    } else if (0 != fseek(fp, 0, SEEK_END)) {
      ret = OB_IO_ERROR;
    } else {
      const long size = ftell(fp);
      if (size < 0 || 0 != fseek(fp, 0, SEEK_SET)) {
        ret = OB_IO_ERROR;
      } else if (OB_ISNULL(data = static_cast<char *>(alloc.alloc(size + 1)))) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else if (static_cast<size_t>(size) != fread(data, 1, static_cast<size_t>(size), fp)) {
        ret = OB_IO_ERROR;
      } else {
        len = size;
        data[len] = '\0';
      }
    }
    if (nullptr != fp) {
      fclose(fp);
    }
  }
  return ret;
}

int open_file_stream(const ObString &path, FILE *&fp)
{
  int ret = OB_SUCCESS;
  char buf[FSQ_MAX_PATH];
  fp = nullptr;
  if (path.length() <= 0 || path.length() >= static_cast<int32_t>(sizeof(buf))) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    MEMCPY(buf, path.ptr(), path.length());
    buf[path.length()] = '\0';
    if (nullptr == (fp = fopen(buf, "rb"))) {
      ret = OB_FILE_NOT_EXIST;
      LOG_WARN("failed to open file", K(ret), K(path));
    }
  }
  return ret;
}

bool pattern_match_impl(const char *pat, const int64_t plen, const char *str, const int64_t slen)
{
  if (plen <= 0) {
    return slen <= 0;
  }
  if ('*' == pat[0]) {
    for (int64_t i = 0; i <= slen; ++i) {
      if (pattern_match_impl(pat + 1, plen - 1, str + i, slen - i)) {
        return true;
      }
    }
    return false;
  } else if ('?' == pat[0]) {
    return slen > 0 && pattern_match_impl(pat + 1, plen - 1, str + 1, slen - 1);
  } else {
    return slen > 0 && pat[0] == str[0]
           && pattern_match_impl(pat + 1, plen - 1, str + 1, slen - 1);
  }
}
bool pattern_match(const ObString &pat, const char *str)
{
  return pattern_match_impl(pat.ptr(), pat.length(), str, static_cast<int64_t>(strlen(str)));
}

// ------------------------------------------------------- CSV record stream

class CsvRecordStream
{
public:
  CsvRecordStream()
    : fp_(nullptr), pos_(0), len_(0), eof_(false), delim_(','), pending_(false), pending_char_(0) {}
  ~CsvRecordStream() { close(); }

  int open(const ObString &path, const char delim)
  {
    int ret = OB_SUCCESS;
    close();
    delim_ = delim;
    if (OB_FAIL(open_file_stream(path, fp_))) {
    } else {
      pos_ = 0;
      len_ = 0;
      eof_ = false;
      pending_ = false;
    }
    return ret;
  }

  void close()
  {
    if (nullptr != fp_) {
      fclose(fp_);
      fp_ = nullptr;
    }
  }

  int read_record(ObIArray<ObString> &fields, ObIArray<bool> &quoted, ObIAllocator &alloc)
  {
    int ret = OB_SUCCESS;
    fields.reuse();
    quoted.reuse();
    char c = 0;
    bool any = false;
    char *field_buf = static_cast<char *>(alloc.alloc(1024));
    int64_t field_cap = 1024;
    int64_t field_len = 0;
    bool in_quotes = false;
    bool field_quoted = false;
    bool record_done = false;
    if (OB_ISNULL(field_buf)) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
    }
    while (OB_SUCC(ret) && !record_done) {
      const int got = next_char(c);
      if (0 == got) {
        if (in_quotes) {
          ret = OB_INVALID_ARGUMENT;
          LOG_WARN("unterminated quoted csv field");
        } else if (any || field_len > 0 || field_quoted) {
          if (OB_FAIL(finish_field(fields, quoted, alloc, field_buf, field_len, field_quoted))) {
          } else {
            record_done = true;
          }
        } else {
          ret = OB_ITER_END;
        }
      } else {
        any = true;
        if (in_quotes) {
          if ('"' == c) {
            char nc = 0;
            if (next_char(nc)) {
              if ('"' == nc) {
                if (OB_FAIL(append_char(alloc, field_buf, field_cap, field_len, '"'))) {}
              } else {
                in_quotes = false;
                push_back(nc);
              }
            } else {
              in_quotes = false;
            }
          } else if (OB_FAIL(append_char(alloc, field_buf, field_cap, field_len, c))) {
          }
        } else if ('"' == c && 0 == field_len && !field_quoted) {
          in_quotes = true;
          field_quoted = true;
        } else if (delim_ == c) {
          if (OB_FAIL(finish_field(fields, quoted, alloc, field_buf, field_len, field_quoted))) {}
        } else if ('\n' == c) {
          if (OB_FAIL(finish_field(fields, quoted, alloc, field_buf, field_len, field_quoted))) {
          } else {
            record_done = true;
          }
        } else if ('\r' == c) {
          char nc = 0;
          if (next_char(nc)) {
            if ('\n' != nc) {
              push_back(nc);
            }
          }
          if (OB_FAIL(finish_field(fields, quoted, alloc, field_buf, field_len, field_quoted))) {
          } else {
            record_done = true;
          }
        } else {
          if (OB_FAIL(append_char(alloc, field_buf, field_cap, field_len, c))) {}
        }
      }
    }
    return ret;
  }

private:
  int next_char(char &c)
  {
    int res = 0;
    if (pending_) {
      pending_ = false;
      c = pending_char_;
      res = 1;
    } else if (pos_ < len_) {
      c = buf_[pos_++];
      res = 1;
    } else if (!eof_) {
      len_ = static_cast<int64_t>(fread(buf_, 1, sizeof(buf_), fp_));
      pos_ = 0;
      if (len_ <= 0) {
        eof_ = true;
        res = 0;
      } else {
        c = buf_[pos_++];
        res = 1;
      }
    }
    return res;
  }
  void push_back(const char c)
  {
    pending_ = true;
    pending_char_ = c;
  }
  int append_char(ObIAllocator &alloc, char *&buf, int64_t &cap, int64_t &len, const char c)
  {
    int ret = OB_SUCCESS;
    if (len >= cap) {
      char *new_buf = static_cast<char *>(alloc.alloc(cap * 2));
      if (OB_ISNULL(new_buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        MEMCPY(new_buf, buf, len);
        buf = new_buf;
        cap *= 2;
      }
    }
    if (OB_SUCC(ret)) {
      buf[len++] = c;
    }
    return ret;
  }
  int finish_field(ObIArray<ObString> &fields, ObIArray<bool> &quoted, ObIAllocator &alloc,
                   char *buf, int64_t &len, bool &field_quoted)
  {
    int ret = OB_SUCCESS;
    ObString s;
    if (OB_FAIL(copy_string(alloc, buf, len, s))) {
    } else if (OB_FAIL(fields.push_back(s))) {
    } else if (OB_FAIL(quoted.push_back(field_quoted))) {
    } else {
      len = 0;
      field_quoted = false;
    }
    return ret;
  }

  FILE *fp_;
  char buf_[FSQ_READ_BUF];
  int64_t pos_;
  int64_t len_;
  bool eof_;
  char delim_;
  bool pending_;
  char pending_char_;
};

// --------------------------------------------------------------- JSON flat

struct JsonValue
{
  enum Kind { JV_NULL, JV_BOOL, JV_INT, JV_DOUBLE, JV_STRING } kind_;
  bool bool_;
  int64_t int_;
  double dbl_;
  ObString str_;
  JsonValue() : kind_(JV_NULL), bool_(false), int_(0), dbl_(0.0), str_() {}
};

class JsonObjectParser
{
public:
  JsonObjectParser(const char *p, const int64_t len) : p_(p), len_(len), pos_(0) {}

  int parse(ObIArray<ObString> &names, ObIArray<JsonValue> &values, ObIAllocator &alloc)
  {
    int ret = OB_SUCCESS;
    skip_ws();
    if (!expect('{')) {
      ret = OB_INVALID_ARGUMENT;
      LOG_WARN("json line is not an object");
    }
    skip_ws();
    while (OB_SUCC(ret)) {
      ObString name;
      JsonValue value;
      skip_ws();
      if (peek() == '}') {
        ++pos_;
        break;
      }
      if (OB_FAIL(parse_string(name, alloc))) {
      } else if (OB_FAIL(names.push_back(name))) {
      } else {
        skip_ws();
        if (!expect(':')) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          skip_ws();
          if (OB_FAIL(parse_value(value, alloc))) {
          } else if (OB_FAIL(values.push_back(value))) {
          }
        }
      }
      if (OB_FAIL(ret)) {
      } else {
        skip_ws();
        const char c = peek();
        if (',' == c) {
          ++pos_;
        } else if ('}' == c) {
          ++pos_;
          break;
        } else {
          ret = OB_INVALID_ARGUMENT;
        }
      }
    }
    if (OB_SUCC(ret)) {
      skip_ws();
      if (pos_ != len_) {
        ret = OB_INVALID_ARGUMENT;
      }
    }
    return ret;
  }

private:
  char peek() const { return pos_ < len_ ? p_[pos_] : '\0'; }
  void skip_ws()
  {
    while (pos_ < len_ && is_space_char(p_[pos_])) {
      ++pos_;
    }
  }
  bool expect(const char c)
  {
    if (pos_ < len_ && p_[pos_] == c) {
      ++pos_;
      return true;
    }
    return false;
  }

  int parse_string(ObString &out, ObIAllocator &alloc)
  {
    int ret = OB_SUCCESS;
    if (!expect('"')) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      char *buf = static_cast<char *>(alloc.alloc(len_ - pos_ + 1));
      int64_t n = 0;
      if (OB_ISNULL(buf)) {
        ret = OB_ALLOCATE_MEMORY_FAILED;
      } else {
        bool closed = false;
        while (pos_ < len_) {
          const char c = p_[pos_++];
          if ('"' == c) {
            closed = true;
            break;
          } else if ('\\' == c && pos_ < len_) {
            const char e = p_[pos_++];
            switch (e) {
              case 'n': buf[n++] = '\n'; break;
              case 't': buf[n++] = '\t'; break;
              case 'r': buf[n++] = '\r'; break;
              case 'b': buf[n++] = '\b'; break;
              case 'f': buf[n++] = '\f'; break;
              case '/': buf[n++] = '/'; break;
              case '\\': buf[n++] = '\\'; break;
              case '"': buf[n++] = '"'; break;
              case 'u': {
                int code = 0;
                for (int i = 0; i < 4 && pos_ < len_; ++i) {
                  const char h = p_[pos_++];
                  code <<= 4;
                  if (h >= '0' && h <= '9') code |= (h - '0');
                  else if (h >= 'a' && h <= 'f') code |= (h - 'a' + 10);
                  else if (h >= 'A' && h <= 'F') code |= (h - 'A' + 10);
                }
                if (code < 0x80) {
                  buf[n++] = static_cast<char>(code);
                } else if (code < 0x800) {
                  buf[n++] = static_cast<char>(0xC0 | (code >> 6));
                  buf[n++] = static_cast<char>(0x80 | (code & 0x3F));
                } else {
                  buf[n++] = static_cast<char>(0xE0 | (code >> 12));
                  buf[n++] = static_cast<char>(0x80 | ((code >> 6) & 0x3F));
                  buf[n++] = static_cast<char>(0x80 | (code & 0x3F));
                }
                break;
              }
              default: buf[n++] = e; break;
            }
          } else {
            buf[n++] = c;
          }
        }
        if (!closed) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          out.assign_ptr(buf, static_cast<int32_t>(n));
        }
      }
    }
    return ret;
  }

  int parse_value(JsonValue &value, ObIAllocator &alloc)
  {
    int ret = OB_SUCCESS;
    const char c = peek();
    if ('"' == c) {
      value.kind_ = JsonValue::JV_STRING;
      ret = parse_string(value.str_, alloc);
    } else if ('{' == c || '[' == c) {
      ret = OB_NOT_SUPPORTED;
      LOG_WARN("nested json value is not supported for file sql");
    } else if (pos_ + 4 <= len_ && 0 == strncmp(p_ + pos_, "true", 4)) {
      pos_ += 4;
      value.kind_ = JsonValue::JV_BOOL;
      value.bool_ = true;
    } else if (pos_ + 5 <= len_ && 0 == strncmp(p_ + pos_, "false", 5)) {
      pos_ += 5;
      value.kind_ = JsonValue::JV_BOOL;
      value.bool_ = false;
    } else if (pos_ + 4 <= len_ && 0 == strncmp(p_ + pos_, "null", 4)) {
      pos_ += 4;
      value.kind_ = JsonValue::JV_NULL;
    } else {
      const int64_t start = pos_;
      if (pos_ < len_ && ('-' == p_[pos_] || '+' == p_[pos_])) {
        ++pos_;
      }
      bool is_double = false;
      while (pos_ < len_) {
        const char d = p_[pos_];
        if (is_digit_char(d)) {
          ++pos_;
        } else if ('.' == d || 'e' == d || 'E' == d || '+' == d || '-' == d) {
          is_double = true;
          ++pos_;
        } else {
          break;
        }
      }
      if (pos_ == start) {
        ret = OB_INVALID_ARGUMENT;
      } else {
        char buf[64];
        const int64_t n = pos_ - start;
        if (n >= static_cast<int64_t>(sizeof(buf))) {
          ret = OB_INVALID_ARGUMENT;
        } else {
          MEMCPY(buf, p_ + start, n);
          buf[n] = '\0';
          if (is_double) {
            value.kind_ = JsonValue::JV_DOUBLE;
            value.dbl_ = strtod(buf, nullptr);
          } else {
            value.kind_ = JsonValue::JV_INT;
            value.int_ = strtoll(buf, nullptr, 10);
          }
        }
      }
    }
    return ret;
  }

  const char *p_;
  int64_t len_;
  int64_t pos_;
};

int json_value_to_cell(const JsonValue &jv, const FileSqlColumn &col,
                       ObIAllocator &alloc, FileSqlValue &val)
{
  int ret = OB_SUCCESS;
  switch (jv.kind_) {
    case JsonValue::JV_NULL:
      val.set_null();
      break;
    case JsonValue::JV_BOOL:
      val.is_null_ = false;
      val.int_val_ = jv.bool_ ? 1 : 0;
      break;
    case JsonValue::JV_INT:
      val.is_null_ = false;
      val.int_val_ = jv.int_;
      if (ObDoubleType == col.type_) {
        val.dbl_val_ = static_cast<double>(jv.int_);
      }
      break;
    case JsonValue::JV_DOUBLE:
      val.is_null_ = false;
      val.dbl_val_ = jv.dbl_;
      val.int_val_ = static_cast<int64_t>(jv.dbl_);
      break;
    case JsonValue::JV_STRING:
    default: {
      if (ob_is_string_type(col.type_)) {
        ObString s;
        if (OB_FAIL(copy_string(alloc, jv.str_.ptr(), jv.str_.length(), s))) {
        } else {
          val.is_null_ = false;
          val.str_val_ = s;
        }
      } else if (OB_FAIL(parse_text_cell(jv.str_.ptr(), jv.str_.length(), col, val))) {
      }
      break;
    }
  }
  return ret;
}

// ---------------------------------------------------------------- Parquet

enum TType
{
  TT_STOP = 0, TT_TRUE = 1, TT_FALSE = 2, TT_BYTE = 3, TT_I16 = 4, TT_I32 = 5,
  TT_I64 = 6, TT_DOUBLE = 7, TT_BINARY = 8, TT_LIST = 9, TT_SET = 10, TT_MAP = 11,
  TT_STRUCT = 12,
};

class CompactReader
{
public:
  CompactReader() : buf_(nullptr), len_(0), pos_(0) {}
  void reset(const char *buf, const int64_t len)
  {
    buf_ = buf;
    len_ = len;
    pos_ = 0;
  }
  bool ok() const { return nullptr != buf_ && pos_ <= len_; }
  int64_t pos() const { return pos_; }
  void set_pos(const int64_t pos) { pos_ = pos; }
  int64_t remain() const { return len_ - pos_; }

  int read_byte(uint8_t &v)
  {
    int ret = OB_SUCCESS;
    if (pos_ >= len_) {
      ret = OB_INVALID_DATA;
    } else {
      v = static_cast<uint8_t>(buf_[pos_++]);
    }
    return ret;
  }
  int read_varint(uint64_t &v)
  {
    int ret = OB_SUCCESS;
    v = 0;
    int shift = 0;
    bool done = false;
    while (OB_SUCC(ret) && !done) {
      uint8_t c = 0;
      if (OB_FAIL(read_byte(c))) {
      } else {
        v |= static_cast<uint64_t>(c & 0x7f) << shift;
        if (0 == (c & 0x80)) {
          done = true;
        } else {
          shift += 7;
          if (shift > 63) {
            ret = OB_INVALID_DATA;
          }
        }
      }
    }
    return ret;
  }
  int read_zigzag(int64_t &v)
  {
    int ret = OB_SUCCESS;
    uint64_t n = 0;
    if (OB_FAIL(read_varint(n))) {
    } else {
      v = static_cast<int64_t>((n >> 1) ^ (~(n & 1) + 1));
    }
    return ret;
  }
  int read_binary(ObString &out)
  {
    int ret = OB_SUCCESS;
    uint64_t n = 0;
    if (OB_FAIL(read_varint(n))) {
    } else if (pos_ + static_cast<int64_t>(n) > len_) {
      ret = OB_INVALID_DATA;
    } else {
      out.assign_ptr(buf_ + pos_, static_cast<int32_t>(n));
      pos_ += n;
    }
    return ret;
  }
  int read_field_header(const uint8_t h, int64_t &last_id, int64_t &fid, uint8_t &ftype)
  {
    int ret = OB_SUCCESS;
    ftype = static_cast<uint8_t>(h & 0x0f);
    const int64_t delta = (h >> 4) & 0x0f;
    if (0 == delta) {
      if (OB_FAIL(read_zigzag(fid))) {
      } else {
        last_id = fid;
      }
    } else {
      fid = last_id + delta;
      last_id = fid;
    }
    return ret;
  }
  int skip_struct(const int32_t depth)
  {
    int ret = OB_SUCCESS;
    if (depth > 32) {
      ret = OB_INVALID_DATA;
    } else {
      int64_t last_id = 0;
      bool done = false;
      while (OB_SUCC(ret) && !done) {
        uint8_t h = 0;
        int64_t fid = 0;
        uint8_t ftype = 0;
        if (OB_FAIL(read_byte(h))) {
        } else if (0 == h) {
          done = true;
        } else if (OB_FAIL(read_field_header(h, last_id, fid, ftype))) {
        } else if (OB_FAIL(skip_value(ftype, depth + 1))) {
        }
      }
    }
    return ret;
  }
  int skip_value(const uint8_t ftype, const int32_t depth)
  {
    int ret = OB_SUCCESS;
    switch (ftype) {
      case TT_TRUE:
      case TT_FALSE:
        break;
      case TT_BYTE: {
        uint8_t b = 0;
        ret = read_byte(b);
        break;
      }
      case TT_I16:
      case TT_I32:
      case TT_I64: {
        int64_t v = 0;
        ret = read_zigzag(v);
        break;
      }
      case TT_DOUBLE:
        pos_ += 8;
        if (pos_ > len_) { ret = OB_INVALID_DATA; }
        break;
      case TT_BINARY: {
        ObString s;
        ret = read_binary(s);
        break;
      }
      case TT_LIST:
      case TT_SET: {
        uint8_t h = 0;
        uint64_t size = 0;
        uint8_t etype = 0;
        if (OB_FAIL(read_byte(h))) {
        } else {
          size = (h >> 4) & 0x0f;
          etype = h & 0x0f;
          if (15 == size) {
            if (OB_FAIL(read_varint(size))) {}
          }
          for (uint64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
            ret = skip_value(etype, depth + 1);
          }
        }
        break;
      }
      case TT_MAP: {
        uint64_t size = 0;
        if (OB_FAIL(read_varint(size))) {
        } else if (size > 0) {
          uint8_t h = 0;
          if (OB_FAIL(read_byte(h))) {
          } else {
            const uint8_t kt = (h >> 4) & 0x0f;
            const uint8_t vt = h & 0x0f;
            for (uint64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
              if (OB_FAIL(skip_value(kt, depth + 1))) {
              } else {
                ret = skip_value(vt, depth + 1);
              }
            }
          }
        }
        break;
      }
      case TT_STRUCT:
        ret = skip_struct(depth + 1);
        break;
      default:
        ret = OB_INVALID_DATA;
        break;
    }
    return ret;
  }

private:
  const char *buf_;
  int64_t len_;
  int64_t pos_;
};

int snappy_decompress(const char *in, const int64_t in_len, char *out, const int64_t out_len,
                      int64_t &written)
{
  int ret = OB_SUCCESS;
  int64_t ip = 0;
  int64_t op = 0;
  written = 0;
  uint64_t ulen = 0;
  int shift = 0;
  bool done = false;
  while (ip < in_len && !done) {
    const uint8_t c = static_cast<uint8_t>(in[ip++]);
    ulen |= static_cast<uint64_t>(c & 0x7f) << shift;
    if (0 == (c & 0x80)) {
      done = true;
    } else {
      shift += 7;
    }
  }
  if (!done || static_cast<int64_t>(ulen) > out_len) {
    ret = OB_INVALID_DATA;
  }
  while (OB_SUCC(ret) && ip < in_len && op < static_cast<int64_t>(ulen)) {
    const uint8_t tag = static_cast<uint8_t>(in[ip++]);
    const int64_t t = tag & 3;
    if (0 == t) {
      int64_t n = (tag >> 2) + 1;
      if (n > 60) {
        const int64_t nb = n - 60;
        n = 0;
        for (int64_t i = 0; i < nb && ip < in_len; ++i) {
          n |= static_cast<int64_t>(static_cast<uint8_t>(in[ip++])) << (8 * i);
        }
        n += 1;
      }
      if (ip + n > in_len || op + n > static_cast<int64_t>(ulen)) {
        ret = OB_INVALID_DATA;
      } else {
        MEMCPY(out + op, in + ip, n);
        ip += n;
        op += n;
      }
    } else {
      int64_t n = 0;
      int64_t offset = 0;
      if (1 == t) {
        n = ((tag >> 2) & 0x7) + 4;
        offset = ((tag >> 5) << 8) | static_cast<uint8_t>(in[ip++]);
      } else if (2 == t) {
        n = (tag >> 2) + 1;
        offset = static_cast<uint8_t>(in[ip]) | (static_cast<uint8_t>(in[ip + 1]) << 8);
        ip += 2;
      } else {
        n = (tag >> 2) + 1;
        offset = static_cast<uint8_t>(in[ip]) | (static_cast<uint8_t>(in[ip + 1]) << 8)
                 | (static_cast<uint8_t>(in[ip + 2]) << 16)
                 | (static_cast<uint8_t>(in[ip + 3]) << 24);
        ip += 4;
      }
      if (offset <= 0 || offset > op || op + n > static_cast<int64_t>(ulen)) {
        ret = OB_INVALID_DATA;
      } else {
        for (int64_t i = 0; i < n; ++i) {
          out[op + i] = out[op - offset + i];
        }
        op += n;
      }
    }
  }
  if (OB_SUCC(ret)) {
    written = op;
    if (op != static_cast<int64_t>(ulen)) {
      ret = OB_INVALID_DATA;
    }
  }
  return ret;
}

class RleDecoder
{
public:
  RleDecoder() : p_(nullptr), len_(0), pos_(0), bit_width_(0), cur_value_(0), cur_count_(0),
                 packed_(nullptr), packed_len_(0), packed_pos_(0) {}
  void reset(const char *p, const int64_t len, const int64_t bit_width)
  {
    p_ = p;
    len_ = len;
    pos_ = 0;
    bit_width_ = bit_width;
    cur_count_ = 0;
    packed_len_ = 0;
    packed_pos_ = 0;
  }
  int next(int64_t &value)
  {
    int ret = OB_SUCCESS;
    if (cur_count_ > 0) {
      --cur_count_;
      value = cur_value_;
    } else if (packed_pos_ < packed_len_) {
      value = read_packed_one();
    } else if (OB_FAIL(read_run())) {
    } else {
      ret = next(value);
    }
    return ret;
  }

private:
  int read_run()
  {
    int ret = OB_SUCCESS;
    if (pos_ >= len_) {
      ret = OB_ITER_END;
    } else {
      uint64_t header = 0;
      int shift = 0;
      bool done = false;
      while (!done && pos_ < len_) {
        const uint8_t c = static_cast<uint8_t>(p_[pos_++]);
        header |= static_cast<uint64_t>(c & 0x7f) << shift;
        if (0 == (c & 0x80)) {
          done = true;
        } else {
          shift += 7;
        }
      }
      if (!done) {
        ret = OB_INVALID_DATA;
      } else if (header & 1) {
        const int64_t groups = static_cast<int64_t>(header >> 1);
        packed_len_ = groups * 8;
        packed_pos_ = 0;
        packed_ = p_ + pos_;
        const int64_t bytes = (packed_len_ * bit_width_ + 7) / 8;
        if (pos_ + bytes > len_) {
          ret = OB_INVALID_DATA;
        } else {
          pos_ += bytes;
        }
      } else {
        cur_count_ = static_cast<int64_t>(header >> 1);
        cur_value_ = 0;
        const int64_t nbytes = (bit_width_ + 7) / 8;
        if (pos_ + nbytes > len_) {
          ret = OB_INVALID_DATA;
        } else {
          for (int64_t i = 0; i < nbytes; ++i) {
            cur_value_ |= static_cast<int64_t>(static_cast<uint8_t>(p_[pos_ + i])) << (8 * i);
          }
          pos_ += nbytes;
        }
      }
    }
    return ret;
  }
  int64_t read_packed_one()
  {
    int64_t value = 0;
    const int64_t bit = packed_pos_ * bit_width_;
    for (int64_t i = 0; i < bit_width_; ++i) {
      const int64_t byte_idx = (bit + i) / 8;
      const int64_t bit_idx = (bit + i) % 8;
      if (static_cast<uint8_t>(packed_[byte_idx]) & (1 << bit_idx)) {
        value |= (1LL << i);
      }
    }
    ++packed_pos_;
    return value;
  }

  const char *p_;
  int64_t len_;
  int64_t pos_;
  int64_t bit_width_;
  int64_t cur_value_;
  int64_t cur_count_;
  const char *packed_;
  int64_t packed_len_;
  int64_t packed_pos_;
};

struct PqColumnMeta
{
  PqColumnMeta()
    : type_(0), codec_(0), num_values_(0), total_compressed_(0), total_uncompressed_(0),
      data_page_offset_(0), dict_page_offset_(-1), name_() {}
  int32_t type_;
  int32_t codec_;
  int64_t num_values_;
  int64_t total_compressed_;
  int64_t total_uncompressed_;
  int64_t data_page_offset_;
  int64_t dict_page_offset_;
  ObString name_;
};

struct PqRowGroup
{
  PqRowGroup() : num_rows_(0), columns_() {}
  int64_t num_rows_;
  ObSEArray<PqColumnMeta, 8> columns_;
};

struct PqSchemaColumn
{
  PqSchemaColumn() : physical_type_(0), repetition_(0), converted_type_(0), name_() {}
  int32_t physical_type_;
  int32_t repetition_;
  int32_t converted_type_;
  ObString name_;
};

struct PqFile
{
  PqFile() : num_rows_(0), columns_(), row_groups_() {}
  int64_t num_rows_;
  ObSEArray<PqSchemaColumn, 8> columns_;
  ObSEArray<PqRowGroup, 4> row_groups_;
};

enum PqType
{
  PQ_BOOLEAN = 0, PQ_INT32 = 1, PQ_INT64 = 2, PQ_INT96 = 3, PQ_FLOAT = 4,
  PQ_DOUBLE = 5, PQ_BYTE_ARRAY = 6, PQ_FIXED_LEN_BYTE_ARRAY = 7,
};
enum PqEncodingKind
{
  PQE_PLAIN = 0, PQE_PLAIN_DICTIONARY = 2, PQE_RLE = 3, PQE_BIT_PACKED = 4,
  PQE_RLE_DICTIONARY = 8,
};
enum PqConverted
{
  PQ_CT_UTF8 = 0, PQ_CT_DATE = 6, PQ_CT_TIMESTAMP_MILLIS = 9, PQ_CT_TIMESTAMP_MICROS = 10,
};
enum PqPageType
{
  PQT_DATA_PAGE = 0, PQT_INDEX_PAGE = 1, PQT_DICTIONARY_PAGE = 2, PQT_DATA_PAGE_V2 = 3,
};

int parse_parquet_schema_element(CompactReader &r, PqSchemaColumn &col)
{
  int ret = OB_SUCCESS;
  int64_t last_id = 0;
  bool done = false;
  while (OB_SUCC(ret) && !done) {
    uint8_t h = 0;
    int64_t fid = 0;
    uint8_t ftype = 0;
    if (OB_FAIL(r.read_byte(h))) {
    } else if (0 == h) {
      done = true;
    } else if (OB_FAIL(r.read_field_header(h, last_id, fid, ftype))) {
    } else if (1 == fid) {
      int64_t v = 0;
      if (OB_FAIL(r.read_zigzag(v))) {
      } else {
        col.physical_type_ = static_cast<int32_t>(v);
      }
    } else if (3 == fid) {
      int64_t v = 0;
      if (OB_FAIL(r.read_zigzag(v))) {
      } else {
        col.repetition_ = static_cast<int32_t>(v);
      }
    } else if (4 == fid) {
      ret = r.read_binary(col.name_);
    } else if (6 == fid) {
      int64_t v = 0;
      if (OB_FAIL(r.read_zigzag(v))) {
      } else {
        col.converted_type_ = static_cast<int32_t>(v);
      }
    } else if (OB_FAIL(r.skip_value(ftype, 0))) {
    }
  }
  return ret;
}

int parse_parquet_column_meta(CompactReader &r, PqColumnMeta &col)
{
  int ret = OB_SUCCESS;
  int64_t last_id = 0;
  bool done = false;
  while (OB_SUCC(ret) && !done) {
    uint8_t h = 0;
    int64_t fid = 0;
    uint8_t ftype = 0;
    if (OB_FAIL(r.read_byte(h))) {
    } else if (0 == h) {
      done = true;
    } else if (OB_FAIL(r.read_field_header(h, last_id, fid, ftype))) {
    } else if (1 == fid) {
      int64_t v = 0;
      if (OB_FAIL(r.read_zigzag(v))) {
      } else {
        col.type_ = static_cast<int32_t>(v);
      }
    } else if (4 == fid) {
      int64_t v = 0;
      if (OB_FAIL(r.read_zigzag(v))) {
      } else {
        col.codec_ = static_cast<int32_t>(v);
      }
    } else if (5 == fid) {
      int64_t v = 0;
      if (OB_FAIL(r.read_zigzag(v))) {
      } else {
        col.num_values_ = v;
      }
    } else if (6 == fid) {
      int64_t v = 0;
      if (OB_FAIL(r.read_zigzag(v))) {
      } else {
        col.total_uncompressed_ = v;
      }
    } else if (7 == fid) {
      int64_t v = 0;
      if (OB_FAIL(r.read_zigzag(v))) {
      } else {
        col.total_compressed_ = v;
      }
    } else if (9 == fid) {
      int64_t v = 0;
      if (OB_FAIL(r.read_zigzag(v))) {
      } else {
        col.data_page_offset_ = v;
      }
    } else if (11 == fid) {
      int64_t v = 0;
      if (OB_FAIL(r.read_zigzag(v))) {
      } else {
        col.dict_page_offset_ = v;
      }
    } else if (3 == fid) {
      uint8_t lh = 0;
      uint64_t size = 0;
      if (OB_FAIL(r.read_byte(lh))) {
      } else {
        size = (lh >> 4) & 0x0f;
        if (15 == size) {
          if (OB_FAIL(r.read_varint(size))) {}
        }
        for (uint64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
          ObString s;
          if (OB_FAIL(r.read_binary(s))) {
          } else if (i + 1 == size) {
            col.name_ = s;
          }
        }
      }
    } else if (OB_FAIL(r.skip_value(ftype, 0))) {
    }
  }
  return ret;
}

int parse_parquet_row_group(CompactReader &r, PqRowGroup &rg)
{
  int ret = OB_SUCCESS;
  int64_t last_id = 0;
  bool done = false;
  while (OB_SUCC(ret) && !done) {
    uint8_t h = 0;
    int64_t fid = 0;
    uint8_t ftype = 0;
    if (OB_FAIL(r.read_byte(h))) {
    } else if (0 == h) {
      done = true;
    } else if (OB_FAIL(r.read_field_header(h, last_id, fid, ftype))) {
    } else if (1 == fid) {
      uint8_t lh = 0;
      uint64_t size = 0;
      if (OB_FAIL(r.read_byte(lh))) {
      } else {
        size = (lh >> 4) & 0x0f;
        if (15 == size) {
          if (OB_FAIL(r.read_varint(size))) {}
        }
        for (uint64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
          int64_t c_last_id = 0;
          bool c_done = false;
          PqColumnMeta meta;
          while (OB_SUCC(ret) && !c_done) {
            uint8_t ch = 0;
            int64_t cfid = 0;
            uint8_t cftype = 0;
            if (OB_FAIL(r.read_byte(ch))) {
            } else if (0 == ch) {
              c_done = true;
            } else if (OB_FAIL(r.read_field_header(ch, c_last_id, cfid, cftype))) {
            } else if (3 == cfid) {
              ret = parse_parquet_column_meta(r, meta);
            } else if (OB_FAIL(r.skip_value(cftype, 0))) {
            }
          }
          if (OB_SUCC(ret) && OB_FAIL(rg.columns_.push_back(meta))) {
          }
        }
      }
    } else if (3 == fid) {
      int64_t v = 0;
      if (OB_FAIL(r.read_zigzag(v))) {
      } else {
        rg.num_rows_ = v;
      }
    } else if (OB_FAIL(r.skip_value(ftype, 0))) {
    }
  }
  return ret;
}

int parse_parquet_file_meta(const char *buf, const int64_t len, PqFile &file)
{
  int ret = OB_SUCCESS;
  CompactReader r;
  r.reset(buf, len);
  int64_t last_id = 0;
  bool done = false;
  while (OB_SUCC(ret) && !done) {
    uint8_t h = 0;
    int64_t fid = 0;
    uint8_t ftype = 0;
    if (OB_FAIL(r.read_byte(h))) {
    } else if (0 == h) {
      done = true;
    } else if (OB_FAIL(r.read_field_header(h, last_id, fid, ftype))) {
    } else if (2 == fid) {
      uint8_t lh = 0;
      uint64_t size = 0;
      if (OB_FAIL(r.read_byte(lh))) {
      } else {
        size = (lh >> 4) & 0x0f;
        if (15 == size) {
          if (OB_FAIL(r.read_varint(size))) {}
        }
        for (uint64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
          PqSchemaColumn col;
          if (OB_FAIL(parse_parquet_schema_element(r, col))) {
          } else if (0 != i) {
            ret = file.columns_.push_back(col);
          }
        }
      }
    } else if (3 == fid) {
      int64_t v = 0;
      if (OB_FAIL(r.read_zigzag(v))) {
      } else {
        file.num_rows_ = v;
      }
    } else if (4 == fid) {
      uint8_t lh = 0;
      uint64_t size = 0;
      if (OB_FAIL(r.read_byte(lh))) {
      } else {
        size = (lh >> 4) & 0x0f;
        if (15 == size) {
          if (OB_FAIL(r.read_varint(size))) {}
        }
        for (uint64_t i = 0; OB_SUCC(ret) && i < size; ++i) {
          PqRowGroup rg;
          if (OB_FAIL(parse_parquet_row_group(r, rg))) {
          } else if (OB_FAIL(file.row_groups_.push_back(rg))) {
          }
        }
      }
    } else if (OB_FAIL(r.skip_value(ftype, 0))) {
    }
  }
  return ret;
}

ObObjType parquet_obj_type(const PqSchemaColumn &col, bool &is_bool)
{
  ObObjType type = ObVarcharType;
  is_bool = false;
  switch (col.physical_type_) {
    case PQ_BOOLEAN:
      type = ObTinyIntType;
      is_bool = true;
      break;
    case PQ_INT32:
      type = (PQ_CT_DATE == col.converted_type_) ? ObDateType : ObIntType;
      break;
    case PQ_INT64:
      type = (PQ_CT_TIMESTAMP_MILLIS == col.converted_type_
              || PQ_CT_TIMESTAMP_MICROS == col.converted_type_) ? ObDateTimeType : ObIntType;
      break;
    case PQ_FLOAT:
    case PQ_DOUBLE:
      type = ObDoubleType;
      break;
    default:
      type = ObVarcharType;
      break;
  }
  return type;
}

int load_parquet_meta(const ObString &path, ObIAllocator &alloc, PqFile &file)
{
  int ret = OB_SUCCESS;
  char *data = nullptr;
  int64_t len = 0;
  if (OB_FAIL(read_entire_file(path, alloc, data, len))) {
  } else if (len < 12 || 0 != memcmp(data, "PAR1", 4) || 0 != memcmp(data + len - 4, "PAR1", 4)) {
    ret = OB_INVALID_DATA;
    LOG_WARN("not a parquet file", K(ret), K(path));
  } else {
    const int64_t footer_len = static_cast<int64_t>(
        static_cast<uint8_t>(data[len - 8])
        | (static_cast<uint8_t>(data[len - 7]) << 8)
        | (static_cast<uint8_t>(data[len - 6]) << 16)
        | (static_cast<uint8_t>(data[len - 5]) << 24));
    const int64_t footer_start = len - 8 - footer_len;
    if (footer_len <= 0 || footer_start < 4) {
      ret = OB_INVALID_DATA;
    } else if (OB_FAIL(parse_parquet_file_meta(data + footer_start, footer_len, file))) {
    } else if (file.row_groups_.empty() && file.columns_.empty()) {
      ret = OB_INVALID_DATA;
    }
  }
  return ret;
}

struct PqPageInfo
{
  int type_;
  int32_t uncompressed_size_;
  int32_t compressed_size_;
  int32_t num_values_;
  int32_t num_nulls_;
  int32_t encoding_;
  int32_t def_level_len_;
  int32_t rep_level_len_;
  bool is_compressed_;
  PqPageInfo()
    : type_(-1), uncompressed_size_(0), compressed_size_(0), num_values_(0), num_nulls_(0),
      encoding_(0), def_level_len_(0), rep_level_len_(0), is_compressed_(false) {}
};

int parse_parquet_page_header(const char *buf, const int64_t len, PqPageInfo &info,
                              int64_t &header_len)
{
  int ret = OB_SUCCESS;
  CompactReader r;
  r.reset(buf, len);
  int64_t last_id = 0;
  bool done = false;
  while (OB_SUCC(ret) && !done) {
    uint8_t h = 0;
    int64_t fid = 0;
    uint8_t ftype = 0;
    if (OB_FAIL(r.read_byte(h))) {
    } else if (0 == h) {
      done = true;
    } else if (OB_FAIL(r.read_field_header(h, last_id, fid, ftype))) {
    } else if (1 == fid) {
      int64_t v = 0;
      if (OB_FAIL(r.read_zigzag(v))) {
      } else {
        info.type_ = static_cast<int32_t>(v);
      }
    } else if (2 == fid) {
      int64_t v = 0;
      if (OB_FAIL(r.read_zigzag(v))) {
      } else {
        info.uncompressed_size_ = static_cast<int32_t>(v);
      }
    } else if (3 == fid) {
      int64_t v = 0;
      if (OB_FAIL(r.read_zigzag(v))) {
      } else {
        info.compressed_size_ = static_cast<int32_t>(v);
      }
    } else if (5 == fid) {  // data page v1
      int64_t d_last = 0;
      bool d_done = false;
      while (OB_SUCC(ret) && !d_done) {
        uint8_t dh = 0;
        int64_t dfid = 0;
        uint8_t dftype = 0;
        if (OB_FAIL(r.read_byte(dh))) {
        } else if (0 == dh) {
          d_done = true;
        } else if (OB_FAIL(r.read_field_header(dh, d_last, dfid, dftype))) {
        } else if (1 == dfid) {
          int64_t v = 0;
          if (OB_FAIL(r.read_zigzag(v))) {
          } else {
            info.num_values_ = static_cast<int32_t>(v);
          }
        } else if (2 == dfid) {
          int64_t v = 0;
          if (OB_FAIL(r.read_zigzag(v))) {
          } else {
            info.encoding_ = static_cast<int32_t>(v);
          }
        } else if (OB_FAIL(r.skip_value(dftype, 1))) {
        }
      }
    } else if (8 == fid) {  // data page v2
      int64_t d_last = 0;
      bool d_done = false;
      while (OB_SUCC(ret) && !d_done) {
        uint8_t dh = 0;
        int64_t dfid = 0;
        uint8_t dftype = 0;
        if (OB_FAIL(r.read_byte(dh))) {
        } else if (0 == dh) {
          d_done = true;
        } else if (OB_FAIL(r.read_field_header(dh, d_last, dfid, dftype))) {
        } else if (1 == dfid) {
          int64_t v = 0;
          if (OB_FAIL(r.read_zigzag(v))) {
          } else {
            info.num_values_ = static_cast<int32_t>(v);
          }
        } else if (2 == dfid) {
          int64_t v = 0;
          if (OB_FAIL(r.read_zigzag(v))) {
          } else {
            info.num_nulls_ = static_cast<int32_t>(v);
          }
        } else if (4 == dfid) {
          int64_t v = 0;
          if (OB_FAIL(r.read_zigzag(v))) {
          } else {
            info.encoding_ = static_cast<int32_t>(v);
          }
        } else if (5 == dfid) {
          int64_t v = 0;
          if (OB_FAIL(r.read_zigzag(v))) {
          } else {
            info.def_level_len_ = static_cast<int32_t>(v);
          }
        } else if (6 == dfid) {
          int64_t v = 0;
          if (OB_FAIL(r.read_zigzag(v))) {
          } else {
            info.rep_level_len_ = static_cast<int32_t>(v);
          }
        } else if (7 == dfid) {
          info.is_compressed_ = (TT_TRUE == dftype);
          if (TT_TRUE != dftype && TT_FALSE != dftype) {
            int64_t v = 0;
            ret = r.read_zigzag(v);
          }
        } else if (OB_FAIL(r.skip_value(dftype, 1))) {
        }
      }
    } else if (7 == fid) {  // dictionary page
      int64_t d_last = 0;
      bool d_done = false;
      while (OB_SUCC(ret) && !d_done) {
        uint8_t dh = 0;
        int64_t dfid = 0;
        uint8_t dftype = 0;
        if (OB_FAIL(r.read_byte(dh))) {
        } else if (0 == dh) {
          d_done = true;
        } else if (OB_FAIL(r.read_field_header(dh, d_last, dfid, dftype))) {
        } else if (1 == dfid) {
          int64_t v = 0;
          if (OB_FAIL(r.read_zigzag(v))) {
          } else {
            info.num_values_ = static_cast<int32_t>(v);
          }
        } else if (OB_FAIL(r.skip_value(dftype, 1))) {
        }
      }
    } else if (OB_FAIL(r.skip_value(ftype, 0))) {
    }
  }
  if (OB_SUCC(ret)) {
    header_len = r.pos();
  }
  return ret;
}

int decode_plain_values(const char *p, const int64_t len, const int32_t physical_type,
                        const int64_t count, ObIAllocator &alloc,
                        ObIArray<FileSqlValue> &out, bool append)
{
  int ret = OB_SUCCESS;
  int64_t pos = 0;
  UNUSED(append);
  const int64_t fixed_width = (PQ_INT32 == physical_type || PQ_FLOAT == physical_type) ? 4
                              : (PQ_INT64 == physical_type || PQ_DOUBLE == physical_type) ? 8
                              : (PQ_FIXED_LEN_BYTE_ARRAY == physical_type)
                                    ? (count > 0 ? len / count : 0) : 0;
  for (int64_t i = 0; OB_SUCC(ret) && i < count; ++i) {
    FileSqlValue v;
    v.is_null_ = false;
    switch (physical_type) {
      case PQ_BOOLEAN: {
        const int64_t byte_idx = i / 8;
        const int64_t bit_idx = i % 8;
        if (byte_idx >= len) {
          ret = OB_INVALID_DATA;
        } else {
          v.int_val_ = (static_cast<uint8_t>(p[byte_idx]) >> bit_idx) & 1;
        }
        break;
      }
      case PQ_INT32: {
        if (pos + 4 > len) {
          ret = OB_INVALID_DATA;
        } else {
          int32_t x = 0;
          MEMCPY(&x, p + pos, 4);
          pos += 4;
          v.int_val_ = x;
        }
        break;
      }
      case PQ_INT64: {
        if (pos + 8 > len) {
          ret = OB_INVALID_DATA;
        } else {
          int64_t x = 0;
          MEMCPY(&x, p + pos, 8);
          pos += 8;
          v.int_val_ = x;
        }
        break;
      }
      case PQ_FLOAT: {
        if (pos + 4 > len) {
          ret = OB_INVALID_DATA;
        } else {
          float x = 0;
          MEMCPY(&x, p + pos, 4);
          pos += 4;
          v.dbl_val_ = x;
        }
        break;
      }
      case PQ_DOUBLE: {
        if (pos + 8 > len) {
          ret = OB_INVALID_DATA;
        } else {
          double x = 0;
          MEMCPY(&x, p + pos, 8);
          pos += 8;
          v.dbl_val_ = x;
        }
        break;
      }
      case PQ_BYTE_ARRAY:
      case PQ_FIXED_LEN_BYTE_ARRAY: {
        int64_t n = 0;
        if (PQ_FIXED_LEN_BYTE_ARRAY == physical_type) {
          n = fixed_width;
        } else if (pos + 4 > len) {
          ret = OB_INVALID_DATA;
        } else {
          int32_t x = 0;
          MEMCPY(&x, p + pos, 4);
          pos += 4;
          n = x;
        }
        if (OB_SUCC(ret)) {
          if (n < 0 || pos + n > len) {
            ret = OB_INVALID_DATA;
          } else {
            ObString s;
            if (OB_FAIL(copy_string(alloc, p + pos, n, s))) {
            } else {
              v.str_val_ = s;
            }
            pos += n;
          }
        }
        break;
      }
      default:
        ret = OB_NOT_SUPPORTED;
        break;
    }
    if (OB_SUCC(ret)) {
      if (append) {
        ret = out.push_back(v);
      } else {
        ret = out.push_back(v);
      }
    }
  }
  return ret;
}

} // namespace

// =========================================================== public helpers

int check_file_sql_path(const ObString &secure_file_priv, const ObString &path, bool &allowed)
{
  int ret = OB_SUCCESS;
  allowed = true;
  if (path.empty()) {
    allowed = false;
  } else {
    for (int64_t i = 0; i + 1 < path.length() && allowed; ++i) {
      if ('.' == path[i] && '.' == path[i + 1]
          && (0 == i || '/' == path[i - 1])
          && (i + 2 >= path.length() || '/' == path[i + 2])) {
        allowed = false;
      }
    }
    if (allowed && !secure_file_priv.empty()) {
      const bool is_null_str = (4 == secure_file_priv.length()
                                && 0 == strncasecmp(secure_file_priv.ptr(), "NULL", 4));
      if (!is_null_str) {
        bool prefix = secure_file_priv.length() <= path.length();
        for (int64_t i = 0; prefix && i < secure_file_priv.length(); ++i) {
          prefix = (secure_file_priv.ptr()[i] == path.ptr()[i]);
        }
        allowed = prefix;
      }
    }
  }
  return ret;
}

const char *file_sql_type_name(const ObObjType type, const bool is_bool)
{
  const char *name = "VARCHAR";
  if (is_bool) {
    name = "BOOLEAN";
  } else {
    switch (type) {
      case ObIntType: name = "BIGINT"; break;
      case ObTinyIntType: name = "TINYINT"; break;
      case ObSmallIntType: name = "SMALLINT"; break;
      case ObMediumIntType: name = "MEDIUMINT"; break;
      case ObInt32Type: name = "INT"; break;
      case ObUInt64Type: name = "UBIGINT"; break;
      case ObFloatType: name = "FLOAT"; break;
      case ObDoubleType: name = "DOUBLE"; break;
      case ObNumberType: name = "DECIMAL"; break;
      case ObDateType: name = "DATE"; break;
      case ObDateTimeType: name = "DATETIME"; break;
      case ObTimestampType: name = "TIMESTAMP"; break;
      default: name = "VARCHAR"; break;
    }
  }
  return name;
}

int file_sql_value_to_datum(const FileSqlValue &value,
                            const ObObjType target_type,
                            const ObCollationType cs_type,
                            ObIAllocator &alloc,
                            ObDatum &datum)
{
  int ret = OB_SUCCESS;
  UNUSED(cs_type);
  if (value.is_null_) {
    datum.set_null();
  } else {
    switch (target_type) {
      case ObTinyIntType:
      case ObSmallIntType:
      case ObMediumIntType:
      case ObInt32Type:
      case ObIntType:
      case ObUInt64Type:
      case ObBitType:
      case ObEnumType:
      case ObSetType:
        datum.set_int(value.int_val_);
        break;
      case ObFloatType:
      case ObDoubleType:
        datum.set_double(value.dbl_val_);
        break;
      case ObNumberType: {
        char buf[64];
        const int n = snprintf(buf, sizeof(buf), "%ld", static_cast<long>(value.int_val_));
        ObString s;
        if (OB_FAIL(copy_string(alloc, buf, n, s))) {
        } else {
          datum.set_string(s);
        }
        break;
      }
      case ObDateType:
        datum.set_date(static_cast<int32_t>(value.int_val_));
        break;
      case ObDateTimeType:
      case ObTimestampType:
        datum.set_datetime(value.int_val_);
        break;
      default: {
        ObString s;
        if (OB_FAIL(copy_string(alloc, value.str_val_.ptr(), value.str_val_.length(), s))) {
        } else {
          datum.set_string(s);
        }
        break;
      }
    }
  }
  return ret;
}

// =========================================================== schema inference

int infer_file_sql_schema(const ObString &path,
                          const int64_t format,
                          const FileSqlCsvOptions &csv_options,
                          ObIAllocator &alloc,
                          ObIArray<FileSqlColumn> &columns)
{
  int ret = OB_SUCCESS;
  columns.reuse();
  if (FSQ_CSV == format) {
    CsvRecordStream stream;
    ObArenaAllocator tmp_alloc;
    ObSEArray<ObString, 16> fields;
    ObSEArray<bool, 16> quoted;
    ObSEArray<GuessKind, 16> kinds;
    if (OB_FAIL(stream.open(path, csv_options.delimiter_))) {
    } else {
      if (csv_options.has_header_) {
        ret = stream.read_record(fields, quoted, tmp_alloc);
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        } else if (OB_FAIL(ret)) {
        } else {
          for (int64_t c = 0; OB_SUCC(ret) && c < fields.count(); ++c) {
            FileSqlColumn col;
            ObString name;
            const char *p = fields.at(c).ptr();
            int64_t n = fields.at(c).length();
            while (n > 0 && is_space_char(p[0])) { ++p; --n; }
            while (n > 0 && is_space_char(p[n - 1])) { --n; }
            if (OB_FAIL(copy_string(alloc, p, n, name))) {
            } else if (name.empty()) {
              char gen[32];
              const int ln = snprintf(gen, sizeof(gen), "column%ld", static_cast<long>(c));
              ret = copy_string(alloc, gen, ln, name);
            }
            if (OB_SUCC(ret)) {
              ObString candidate = name;
              int64_t suffix = 2;
              while (OB_SUCC(ret)
                     && nullptr != find_column(columns, candidate.ptr(), candidate.length())) {
                char gen[512];
                const int ln = snprintf(gen, sizeof(gen), "%.*s__%ld",
                                        static_cast<int>(name.length()), name.ptr(),
                                        static_cast<long>(suffix));
                ++suffix;
                ret = copy_string(alloc, gen, ln, candidate);
              }
              if (OB_SUCC(ret)) {
                col.name_ = candidate;
                col.id_ = c;
                ret = columns.push_back(col);
              }
            }
          }
          fields.reuse();
          tmp_alloc.reuse();
        }
      }
      for (int64_t i = 0; i < csv_options.skip_header_lines_ && OB_SUCC(ret); ++i) {
        ret = stream.read_record(fields, quoted, tmp_alloc);
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
          break;
        }
        fields.reuse();
        tmp_alloc.reuse();
      }
      int64_t row_count = 0;
      while (OB_SUCC(ret)) {
        ret = stream.read_record(fields, quoted, tmp_alloc);
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
          break;
        } else if (OB_FAIL(ret)) {
          break;
        } else {
          if (0 == columns.count()) {
            for (int64_t c = 0; c < fields.count(); ++c) {
              FileSqlColumn col;
              char gen[32];
              const int ln = snprintf(gen, sizeof(gen), "column%ld", static_cast<long>(c));
              if (OB_FAIL(copy_string(alloc, gen, ln, col.name_))) {
              } else {
                col.id_ = c;
                ret = columns.push_back(col);
              }
            }
          }
          if (OB_SUCC(ret) && kinds.count() != columns.count()) {
            kinds.reuse();
            for (int64_t c = 0; c < columns.count(); ++c) {
              if (OB_FAIL(kinds.push_back(GK_UNKNOWN))) {}
            }
          }
          for (int64_t c = 0; OB_SUCC(ret) && c < columns.count(); ++c) {
            FileSqlColumn &col = columns.at(c);
            if (c >= fields.count()) {
              col.nullable_ = true;
            } else if (fields.at(c).empty() && !quoted.at(c)) {
              col.nullable_ = true;
            } else {
              const char *p = fields.at(c).ptr();
              const int64_t n = fields.at(c).length();
              kinds.at(c) = merge_guess(kinds.at(c), guess_text_type(p, n));
              if (n > col.max_len_) {
                col.max_len_ = n;
              }
            }
          }
          ++row_count;
          if (csv_options.sample_size_ >= 0 && row_count >= csv_options.sample_size_) {
            break;
          }
          fields.reuse();
          tmp_alloc.reuse();
        }
      }
      for (int64_t c = 0; OB_SUCC(ret) && c < columns.count(); ++c) {
        FileSqlColumn &col = columns.at(c);
        bool is_bool = false;
        const GuessKind kind = (c < kinds.count()) ? kinds.at(c) : GK_UNKNOWN;
        col.type_ = guess_to_obj_type(kind, is_bool);
        col.is_bool_ = is_bool;
        if (ObVarcharType == col.type_ && col.max_len_ <= 0) {
          col.max_len_ = 1;
        }
      }
      stream.close();
    }
  } else if (FSQ_JSON == format) {
    char *data = nullptr;
    int64_t len = 0;
    ObSEArray<GuessKind, 16> kinds;
    if (OB_FAIL(read_entire_file(path, alloc, data, len))) {
    } else {
      ObArenaAllocator tmp_alloc;
      int64_t pos = 0;
      ObSEArray<ObString, 16> names;
      ObSEArray<JsonValue, 16> values;
      while (OB_SUCC(ret) && pos < len) {
        int64_t eol = pos;
        while (eol < len && '\n' != data[eol]) {
          ++eol;
        }
        int64_t line_end = eol;
        if (line_end > pos && '\r' == data[line_end - 1]) {
          --line_end;
        }
        if (line_end > pos) {
          JsonObjectParser parser(data + pos, line_end - pos);
          names.reuse();
          values.reuse();
          tmp_alloc.reuse();
          if (OB_FAIL(parser.parse(names, values, tmp_alloc))) {
            LOG_WARN("failed to parse json line", K(ret));
          } else {
            for (int64_t i = 0; OB_SUCC(ret) && i < names.count(); ++i) {
              FileSqlColumn *col = find_column(columns, names.at(i).ptr(), names.at(i).length());
              int64_t ci = -1;
              if (nullptr == col) {
                FileSqlColumn new_col;
                if (OB_FAIL(copy_string(alloc, names.at(i).ptr(), names.at(i).length(),
                                        new_col.name_))) {
                } else {
                  new_col.id_ = columns.count();
                  if (OB_FAIL(columns.push_back(new_col))) {
                  } else if (OB_FAIL(kinds.push_back(GK_UNKNOWN))) {
                  } else {
                    ci = columns.count() - 1;
                  }
                }
              } else {
                ci = col - &columns.at(0);
              }
              if (OB_SUCC(ret) && ci >= 0 && ci < kinds.count()) {
                const JsonValue &v = values.at(i);
                GuessKind kind = GK_UNKNOWN;
                switch (v.kind_) {
                  case JsonValue::JV_NULL:
                    columns.at(ci).nullable_ = true;
                    break;
                  case JsonValue::JV_BOOL: kind = GK_BOOL; break;
                  case JsonValue::JV_INT: kind = GK_INT; break;
                  case JsonValue::JV_DOUBLE: kind = GK_DOUBLE; break;
                  case JsonValue::JV_STRING:
                    kind = guess_text_type(v.str_.ptr(), v.str_.length());
                    if (v.str_.length() > columns.at(ci).max_len_) {
                      columns.at(ci).max_len_ = v.str_.length();
                    }
                    break;
                  default: break;
                }
                if (GK_UNKNOWN != kind) {
                  kinds.at(ci) = merge_guess(kinds.at(ci), kind);
                }
              }
            }
          }
        }
        pos = eol + 1;
      }
      for (int64_t c = 0; OB_SUCC(ret) && c < columns.count(); ++c) {
        FileSqlColumn &col = columns.at(c);
        bool is_bool = false;
        const GuessKind kind = (c < kinds.count()) ? kinds.at(c) : GK_UNKNOWN;
        col.type_ = guess_to_obj_type(kind, is_bool);
        col.is_bool_ = is_bool;
        if (ObVarcharType == col.type_ && col.max_len_ <= 0) {
          col.max_len_ = 1;
        }
      }
    }
  } else if (FSQ_GLOB == format) {
    FileSqlColumn col;
    if (OB_FAIL(copy_string(alloc, "file", 4, col.name_))) {
    } else {
      col.type_ = ObVarcharType;
      col.max_len_ = 4096;
      col.nullable_ = true;
      col.id_ = 0;
      ret = columns.push_back(col);
    }
  } else if (FSQ_PARQUET == format) {
    PqFile file;
    if (OB_FAIL(load_parquet_meta(path, alloc, file))) {
      LOG_WARN("failed to read parquet footer", K(ret), K(path));
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < file.columns_.count(); ++i) {
        const PqSchemaColumn &schema_col = file.columns_.at(i);
        FileSqlColumn col;
        bool is_bool = false;
        if (OB_FAIL(copy_string(alloc, schema_col.name_.ptr(), schema_col.name_.length(),
                                col.name_))) {
        } else {
          col.type_ = parquet_obj_type(schema_col, is_bool);
          col.is_bool_ = is_bool;
          col.nullable_ = (1 != schema_col.repetition_);
          col.id_ = i;
          col.max_len_ = (ObVarcharType == col.type_) ? 256 : 0;
          ret = columns.push_back(col);
        }
      }
    }
  } else {
    ret = OB_NOT_SUPPORTED;
  }
  return ret;
}

// ================================================================ reader

struct FileSqlReader::CsvState
{
  CsvState() : stream_() {}
  CsvRecordStream stream_;
};

struct FileSqlReader::JsonState
{
  JsonState() : data_(nullptr), len_(0), pos_(0) {}
  char *data_;
  int64_t len_;
  int64_t pos_;
};

struct FileSqlReader::GlobState
{
  GlobState() : index_(0), files_() {}
  int64_t index_;
  ObSEArray<ObString, 32> files_;
};

struct FileSqlReader::ParquetState
{
  ParquetState()
    : fp_(nullptr), rg_index_(-1), cur_row_(0), rg_rows_(0), rg_ready_(false), col_values_(),
      rg_alloc_() {}
  FILE *fp_;
  PqFile file_;
  int64_t rg_index_;
  int64_t cur_row_;
  int64_t rg_rows_;
  bool rg_ready_;
  ObSEArray<ObSEArray<FileSqlValue, 8> *, 8> col_values_;
  ObArenaAllocator rg_alloc_;
};

FileSqlReader::FileSqlReader()
  : inited_(false), format_(FSQ_INVALID), csv_options_(), path_(), local_alloc_(),
    csv_(nullptr), json_(nullptr), parquet_(nullptr), glob_(nullptr), columns_(nullptr)
{
}

FileSqlReader::~FileSqlReader()
{
  reset();
}

void FileSqlReader::reset()
{
  if (OB_NOT_NULL(csv_)) {
    csv_->stream_.close();
    csv_->~CsvState();
    csv_ = nullptr;
  }
  if (OB_NOT_NULL(json_)) {
    json_->~JsonState();
    json_ = nullptr;
  }
  if (OB_NOT_NULL(parquet_)) {
    if (nullptr != parquet_->fp_) {
      fclose(parquet_->fp_);
      parquet_->fp_ = nullptr;
    }
    parquet_->~ParquetState();
    parquet_ = nullptr;
  }
  if (OB_NOT_NULL(glob_)) {
    glob_->~GlobState();
    glob_ = nullptr;
  }
  local_alloc_.clear();
  inited_ = false;
}

int FileSqlReader::init(const ObString &path,
                        const int64_t format,
                        const FileSqlCsvOptions &csv_options,
                        const ObIArray<FileSqlColumn> &columns,
                        ObIAllocator &alloc)
{
  int ret = OB_SUCCESS;
  UNUSED(alloc);
  reset();
  format_ = format;
  csv_options_ = csv_options;
  columns_ = &columns;
  if (OB_FAIL(copy_string(local_alloc_, path.ptr(), path.length(), path_))) {
  } else if (FSQ_CSV == format) {
    ret = init_csv();
  } else if (FSQ_JSON == format) {
    ret = init_json();
  } else if (FSQ_PARQUET == format) {
    ret = init_parquet();
  } else if (FSQ_GLOB == format) {
    ret = init_glob();
  } else {
    ret = OB_NOT_SUPPORTED;
  }
  if (OB_SUCC(ret)) {
    inited_ = true;
  }
  return ret;
}

int FileSqlReader::init_csv()
{
  int ret = OB_SUCCESS;
  void *buf = local_alloc_.alloc(sizeof(CsvState));
  if (OB_ISNULL(buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    csv_ = new (buf) CsvState();
    if (OB_FAIL(csv_->stream_.open(path_, csv_options_.delimiter_))) {
    } else {
      ObArenaAllocator tmp_alloc;
      ObSEArray<ObString, 16> fields;
      ObSEArray<bool, 16> quoted;
      int64_t skip = csv_options_.skip_header_lines_;
      if (csv_options_.has_header_) {
        skip += 1;
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < skip; ++i) {
        ret = csv_->stream_.read_record(fields, quoted, tmp_alloc);
        fields.reuse();
        tmp_alloc.reuse();
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
          break;
        }
      }
    }
  }
  return ret;
}

int FileSqlReader::init_json()
{
  int ret = OB_SUCCESS;
  void *buf = local_alloc_.alloc(sizeof(JsonState));
  if (OB_ISNULL(buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    json_ = new (buf) JsonState();
    if (OB_FAIL(read_entire_file(path_, local_alloc_, json_->data_, json_->len_))) {
    }
  }
  return ret;
}

int FileSqlReader::init_glob()
{
  int ret = OB_SUCCESS;
  void *buf = local_alloc_.alloc(sizeof(GlobState));
  if (OB_ISNULL(buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    glob_ = new (buf) GlobState();
#ifdef _WIN32
    ret = OB_NOT_SUPPORTED;
#else
    ObString dir;
    ObString pattern;
    int64_t slash = -1;
    for (int64_t i = 0; i < path_.length(); ++i) {
      if ('/' == path_.ptr()[i]) {
        slash = i;
      }
    }
    if (slash >= 0) {
      dir.assign_ptr(path_.ptr(), static_cast<int32_t>(slash));
      pattern.assign_ptr(path_.ptr() + slash + 1,
                         static_cast<int32_t>(path_.length() - slash - 1));
    } else {
      dir.assign_ptr(".", 1);
      pattern = path_;
    }
    char dir_buf[FSQ_MAX_PATH];
    if (dir.length() <= 0 || dir.length() >= static_cast<int32_t>(sizeof(dir_buf))) {
      ret = OB_INVALID_ARGUMENT;
    } else {
      MEMCPY(dir_buf, dir.ptr(), dir.length());
      dir_buf[dir.length()] = '\0';
      DIR *d = opendir(dir_buf);
      if (nullptr == d) {
        ret = OB_FILE_NOT_EXIST;
        LOG_WARN("failed to open directory", K(ret), K(dir));
      } else {
        struct dirent *ent = nullptr;
        while (OB_SUCC(ret) && nullptr != (ent = readdir(d))) {
          const char *name = ent->d_name;
          if (0 == strcmp(name, ".") || 0 == strcmp(name, "..")) {
            continue;
          }
          if (!pattern_match(pattern, name)) {
            continue;
          }
          const int64_t name_len = static_cast<int64_t>(strlen(name));
          if (dir.length() + 1 + name_len >= FSQ_MAX_PATH) {
            continue;
          }
          char full[FSQ_MAX_PATH];
          MEMCPY(full, dir.ptr(), dir.length());
          int64_t off = dir.length();
          full[off++] = '/';
          MEMCPY(full + off, name, name_len);
          ObString s;
          if (OB_FAIL(copy_string(local_alloc_, full, off + name_len, s))) {
          } else if (OB_FAIL(glob_->files_.push_back(s))) {
          }
        }
        closedir(d);
      }
    }
#endif
  }
  return ret;
}

int FileSqlReader::init_parquet()
{
  int ret = OB_SUCCESS;
  void *buf = local_alloc_.alloc(sizeof(ParquetState));
  if (OB_ISNULL(buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    parquet_ = new (buf) ParquetState();
    if (OB_FAIL(open_file_stream(path_, parquet_->fp_))) {
    } else if (OB_FAIL(load_parquet_meta(path_, local_alloc_, parquet_->file_))) {
      LOG_WARN("failed to read parquet footer", K(ret), K(path_));
    }
  }
  return ret;
}

int FileSqlReader::next_row(ObIAllocator &row_alloc, ObIArray<FileSqlValue> &vals)
{
  int ret = OB_SUCCESS;
  vals.reuse();
  if (!inited_) {
    ret = OB_NOT_INIT;
  } else if (FSQ_CSV == format_) {
    ret = csv_next_row(row_alloc, vals);
  } else if (FSQ_JSON == format_) {
    ret = json_next_row(row_alloc, vals);
  } else if (FSQ_PARQUET == format_) {
    ret = parquet_next_row(row_alloc, vals);
  } else if (FSQ_GLOB == format_) {
    ret = glob_next_row(row_alloc, vals);
  } else {
    ret = OB_NOT_SUPPORTED;
  }
  return ret;
}

int FileSqlReader::csv_next_row(ObIAllocator &row_alloc, ObIArray<FileSqlValue> &vals)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator tmp_alloc;
  ObSEArray<ObString, 16> fields;
  ObSEArray<bool, 16> quoted;
  if (OB_ISNULL(csv_)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    ret = csv_->stream_.read_record(fields, quoted, tmp_alloc);
    if (OB_SUCCESS == ret) {
      for (int64_t c = 0; OB_SUCC(ret) && c < columns_->count(); ++c) {
        FileSqlValue val;
        const FileSqlColumn &col = columns_->at(c);
        if (c >= fields.count() || (fields.at(c).empty() && !quoted.at(c))) {
          val.set_null();
        } else if (OB_FAIL(parse_text_cell(fields.at(c).ptr(), fields.at(c).length(), col, val))) {
          LOG_WARN("failed to convert csv cell", K(ret), K(c));
        } else if (ob_is_string_type(col.type_)) {
          ObString s;
          if (OB_FAIL(copy_string(row_alloc, val.str_val_.ptr(), val.str_val_.length(), s))) {
          } else {
            val.str_val_ = s;
          }
        }
        if (OB_SUCC(ret) && OB_FAIL(vals.push_back(val))) {
        }
      }
    }
  }
  return ret;
}

int FileSqlReader::json_next_row(ObIAllocator &row_alloc, ObIArray<FileSqlValue> &vals)
{
  int ret = OB_SUCCESS;
  ObArenaAllocator tmp_alloc;
  ObSEArray<ObString, 16> names;
  ObSEArray<JsonValue, 16> values;
  bool got_line = false;
  if (OB_ISNULL(json_)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    while (OB_SUCC(ret) && !got_line && json_->pos_ < json_->len_) {
      int64_t eol = json_->pos_;
      while (eol < json_->len_ && '\n' != json_->data_[eol]) {
        ++eol;
      }
      int64_t line_end = eol;
      if (line_end > json_->pos_ && '\r' == json_->data_[line_end - 1]) {
        --line_end;
      }
      if (line_end > json_->pos_) {
        JsonObjectParser parser(json_->data_ + json_->pos_, line_end - json_->pos_);
        names.reuse();
        values.reuse();
        tmp_alloc.reuse();
        if (OB_FAIL(parser.parse(names, values, tmp_alloc))) {
          LOG_WARN("failed to parse json line", K(ret));
        } else {
          got_line = true;
        }
      }
      json_->pos_ = eol + 1;
    }
    if (OB_SUCC(ret)) {
      if (!got_line) {
        ret = OB_ITER_END;
      } else {
        for (int64_t c = 0; OB_SUCC(ret) && c < columns_->count(); ++c) {
          const FileSqlColumn &col = columns_->at(c);
          FileSqlValue val;
          bool found = false;
          for (int64_t i = 0; i < names.count() && !found; ++i) {
            if (names.at(i).length() == col.name_.length()
                && 0 == strncasecmp(names.at(i).ptr(), col.name_.ptr(),
                                    static_cast<size_t>(col.name_.length()))) {
              if (OB_FAIL(json_value_to_cell(values.at(i), col, row_alloc, val))) {
              } else {
                found = true;
              }
            }
          }
          if (OB_SUCC(ret) && !found) {
            val.set_null();
          }
          if (OB_SUCC(ret) && OB_FAIL(vals.push_back(val))) {
          }
        }
      }
    }
  }
  return ret;
}

int FileSqlReader::glob_next_row(ObIAllocator &row_alloc, ObIArray<FileSqlValue> &vals)
{
  int ret = OB_SUCCESS;
  UNUSED(row_alloc);
  if (OB_ISNULL(glob_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (glob_->index_ >= glob_->files_.count()) {
    ret = OB_ITER_END;
  } else {
    FileSqlValue val;
    val.is_null_ = false;
    val.str_val_ = glob_->files_.at(glob_->index_++);
    ret = vals.push_back(val);
  }
  return ret;
}

// ------------------------------------------------- parquet chunk decoding

int pq_decompress_page_body(const char *body, const PqPageInfo &info, const int32_t codec,
                            char *plain, const int64_t plain_cap, int64_t &plain_len)
{
  int ret = OB_SUCCESS;
  if (1 == codec) {  // snappy
    if (info.uncompressed_size_ > plain_cap) {
      ret = OB_INVALID_DATA;
    } else if (OB_FAIL(snappy_decompress(body, info.compressed_size_, plain,
                                         info.uncompressed_size_, plain_len))) {
    }
  } else if (0 == codec) {
    if (info.compressed_size_ > plain_cap) {
      ret = OB_INVALID_DATA;
    } else {
      MEMCPY(plain, body, info.compressed_size_);
      plain_len = info.compressed_size_;
    }
  } else {
    ret = OB_NOT_SUPPORTED;
    LOG_WARN("unsupported parquet compression codec", K(ret), K(codec));
  }
  return ret;
}

int pq_decode_data_page(const PqSchemaColumn &schema_col,
                        const PqColumnMeta &meta,
                        const PqPageInfo &info,
                        const char *body,
                        char *plain,
                        const int64_t plain_cap,
                        const bool has_dict,
                        const ObIArray<FileSqlValue> &dict,
                        ObIAllocator &alloc,
                        ObIArray<FileSqlValue> &out)
{
  int ret = OB_SUCCESS;
  const bool optional = (1 == schema_col.repetition_);
  int64_t def_len = 0;
  const char *def_data = nullptr;
  const char *value_data = nullptr;
  int64_t value_len = 0;
  if (PQT_DATA_PAGE == info.type_) {
    int64_t plain_len = 0;
    if (OB_FAIL(pq_decompress_page_body(body, info, meta.codec_, plain, plain_cap, plain_len))) {
    } else if (optional) {
      if (plain_len < 4) {
        ret = OB_INVALID_DATA;
      } else {
        int32_t l = 0;
        MEMCPY(&l, plain, 4);
        if (l < 0 || 4 + l > plain_len) {
          ret = OB_INVALID_DATA;
        } else {
          def_len = l;
          def_data = plain + 4;
          value_data = plain + 4 + l;
          value_len = plain_len - 4 - l;
        }
      }
    } else {
      value_data = plain;
      value_len = plain_len;
    }
  } else {
    const int64_t levels_len = info.rep_level_len_ + info.def_level_len_;
    def_len = info.def_level_len_;
    def_data = body + info.rep_level_len_;
    if (levels_len > info.compressed_size_) {
      ret = OB_INVALID_DATA;
    } else if (info.is_compressed_) {
      int64_t plain_len = 0;
      if (OB_FAIL(snappy_decompress(body + levels_len, info.compressed_size_ - levels_len,
                                    plain, plain_cap, plain_len))) {
      } else {
        value_data = plain;
        value_len = plain_len;
      }
    } else {
      value_data = body + levels_len;
      value_len = info.compressed_size_ - levels_len;
    }
  }
  if (OB_FAIL(ret)) {
  } else {
    const int64_t total = info.num_values_;
    int64_t non_null = 0;
    ObSEArray<bool, 128> present;
    if (optional && def_len > 0) {
      RleDecoder decoder;
      decoder.reset(def_data, def_len, 1);
      for (int64_t i = 0; OB_SUCC(ret) && i < total; ++i) {
        int64_t level = 0;
        const int dret = decoder.next(level);
        if (OB_ITER_END == dret) {
          for (int64_t j = i; OB_SUCC(ret) && j < total; ++j) {
            if (OB_FAIL(present.push_back(true))) {}
          }
          break;
        } else if (OB_FAIL(dret)) {
          ret = dret;
        } else if (OB_FAIL(present.push_back(level > 0))) {
        } else if (level > 0) {
          ++non_null;
        }
      }
    } else {
      non_null = total;
      for (int64_t i = 0; OB_SUCC(ret) && i < total; ++i) {
        if (OB_FAIL(present.push_back(true))) {}
      }
    }
    if (OB_FAIL(ret)) {
    } else {
      ObSEArray<FileSqlValue, 8> plain_values;
      const int32_t encoding = info.encoding_;
      if (PQE_RLE_DICTIONARY == encoding || PQE_PLAIN_DICTIONARY == encoding) {
        if (!has_dict || value_len < 1) {
          ret = OB_INVALID_DATA;
        } else {
          const int64_t bit_width = static_cast<uint8_t>(value_data[0]);
          RleDecoder decoder;
          decoder.reset(value_data + 1, value_len - 1, bit_width);
          for (int64_t i = 0; OB_SUCC(ret) && i < non_null; ++i) {
            int64_t idx = 0;
            if (OB_FAIL(decoder.next(idx))) {
            } else if (idx < 0 || idx >= dict.count()) {
              ret = OB_INVALID_DATA;
            } else if (OB_FAIL(plain_values.push_back(dict.at(idx)))) {
            }
          }
        }
      } else if (PQE_PLAIN == encoding) {
        ret = decode_plain_values(value_data, value_len, meta.type_, non_null, alloc,
                                  plain_values, false);
      } else {
        ret = OB_NOT_SUPPORTED;
        LOG_WARN("unsupported parquet encoding", K(ret), K(encoding));
      }
      if (OB_SUCC(ret)) {
        int64_t vi = 0;
        for (int64_t i = 0; OB_SUCC(ret) && i < present.count(); ++i) {
          if (present.at(i)) {
            if (vi < plain_values.count()) {
              ret = out.push_back(plain_values.at(vi++));
            } else {
              ret = OB_INVALID_DATA;
            }
          } else {
            FileSqlValue v;
            v.set_null();
            ret = out.push_back(v);
          }
        }
      }
    }
  }
  return ret;
}

int pq_decode_column(FILE *fp, const PqSchemaColumn &schema_col, const PqColumnMeta &meta,
                     ObIAllocator &alloc, ObSEArray<FileSqlValue, 8> *&out)
{
  int ret = OB_SUCCESS;
  int64_t page_pos = (meta.dict_page_offset_ >= 0) ? meta.dict_page_offset_
                                                   : meta.data_page_offset_;
  int64_t produced = 0;
  bool has_dict = false;
  ObSEArray<FileSqlValue, 8> dict;
  ObSEArray<FileSqlValue, 8> *values = nullptr;
  void *buf = alloc.alloc(sizeof(ObSEArray<FileSqlValue, 8>));
  if (OB_ISNULL(buf)) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    values = new (buf) ObSEArray<FileSqlValue, 8>();
  }
  while (OB_SUCC(ret) && produced < meta.num_values_) {
    if (0 != fseek(fp, page_pos, SEEK_SET)) {
      ret = OB_IO_ERROR;
    } else {
      PqPageInfo info;
      char header_buf[8192];
      const size_t nread = fread(header_buf, 1, sizeof(header_buf), fp);
      int64_t header_len = 0;
      if (0 == nread) {
        ret = OB_INVALID_DATA;
      } else if (OB_FAIL(parse_parquet_page_header(header_buf, static_cast<int64_t>(nread),
                                                  info, header_len))) {
      } else if (info.compressed_size_ <= 0 || info.uncompressed_size_ < 0) {
        ret = OB_INVALID_DATA;
      } else {
        page_pos += header_len;
        char *body = static_cast<char *>(alloc.alloc(info.compressed_size_));
        char *plain = static_cast<char *>(alloc.alloc(info.uncompressed_size_ + 64));
        if (OB_ISNULL(body) || OB_ISNULL(plain)) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else if (0 != fseek(fp, page_pos, SEEK_SET)
                   || static_cast<size_t>(info.compressed_size_)
                        != fread(body, 1, info.compressed_size_, fp)) {
          ret = OB_IO_ERROR;
        } else {
          page_pos += info.compressed_size_;
          if (PQT_DICTIONARY_PAGE == info.type_) {
            int64_t plain_len = 0;
            if (OB_FAIL(pq_decompress_page_body(body, info, meta.codec_, plain,
                                                info.uncompressed_size_ + 64, plain_len))) {
            } else if (OB_FAIL(decode_plain_values(plain, plain_len, meta.type_,
                                                   info.num_values_, alloc, dict, false))) {
            } else {
              has_dict = true;
            }
          } else if (PQT_DATA_PAGE == info.type_ || PQT_DATA_PAGE_V2 == info.type_) {
            if (OB_FAIL(pq_decode_data_page(schema_col, meta, info, body, plain,
                                            info.uncompressed_size_ + 64,
                                            has_dict, dict, alloc, *values))) {
            } else {
              produced += info.num_values_;
            }
          } else {
            // skip unknown pages
          }
        }
      }
    }
  }
  if (OB_SUCC(ret)) {
    out = values;
  }
  return ret;
}

int FileSqlReader::parquet_next_row(ObIAllocator &row_alloc, ObIArray<FileSqlValue> &vals)
{
  int ret = OB_SUCCESS;
  UNUSED(row_alloc);
  if (OB_ISNULL(parquet_)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    while (OB_SUCC(ret)) {
      if (!parquet_->rg_ready_ || parquet_->cur_row_ >= parquet_->rg_rows_) {
        ++parquet_->rg_index_;
        parquet_->rg_ready_ = false;
        parquet_->col_values_.reuse();
        parquet_->rg_alloc_.reuse();
        if (parquet_->rg_index_ >= parquet_->file_.row_groups_.count()) {
          ret = OB_ITER_END;
          break;
        }
        const PqRowGroup &rg = parquet_->file_.row_groups_.at(parquet_->rg_index_);
        parquet_->rg_rows_ = rg.num_rows_;
        parquet_->cur_row_ = 0;
        for (int64_t c = 0; OB_SUCC(ret) && c < rg.columns_.count(); ++c) {
          ObSEArray<FileSqlValue, 8> *col_values = nullptr;
          const PqSchemaColumn &schema_col =
              (c < parquet_->file_.columns_.count()) ? parquet_->file_.columns_.at(c)
                                                     : PqSchemaColumn();
          if (OB_FAIL(pq_decode_column(parquet_->fp_, schema_col, rg.columns_.at(c),
                                        parquet_->rg_alloc_, col_values))) {
          } else if (OB_FAIL(parquet_->col_values_.push_back(col_values))) {
          }
        }
        if (OB_SUCC(ret)) {
          parquet_->rg_ready_ = true;
        }
      } else {
        break;
      }
    }
    if (OB_SUCC(ret)) {
      if (parquet_->cur_row_ >= parquet_->rg_rows_) {
        ret = OB_ITER_END;
      } else {
        for (int64_t c = 0; OB_SUCC(ret) && c < parquet_->col_values_.count(); ++c) {
          const ObSEArray<FileSqlValue, 8> *col_vals = parquet_->col_values_.at(c);
          FileSqlValue v;
          if (parquet_->cur_row_ < col_vals->count()) {
            v = col_vals->at(parquet_->cur_row_);
          }
          if (OB_FAIL(vals.push_back(v))) {
          }
        }
        if (OB_SUCC(ret)) {
          ++parquet_->cur_row_;
        }
      }
    }
  }
  return ret;
}

} // namespace filesql
} // namespace sql
} // namespace oceanbase
