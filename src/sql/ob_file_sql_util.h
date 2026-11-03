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

// File SQL support: table functions that read local files (CSV / JSONL /
// Parquet / glob).  This header is shared between the resolver (schema
// inference and validation) and the executor (streaming row scan).

#ifndef OCEANBASE_SQL_OB_FILE_SQL_UTIL_H_
#define OCEANBASE_SQL_OB_FILE_SQL_UTIL_H_

#include "lib/string/ob_string.h"
#include "lib/container/ob_se_array.h"
#include "lib/allocator/ob_allocator.h"
#include "lib/allocator/page_arena.h"
#include "common/object/ob_object.h"
#include "common/datum/ob_datum.h"

namespace oceanbase
{
namespace sql
{
namespace filesql
{

enum FileSqlFormat : int64_t
{
  FSQ_INVALID = 0,
  FSQ_CSV = 1,
  FSQ_JSON = 2,
  FSQ_PARQUET = 3,
  FSQ_GLOB = 4,
};

// One inferred column of a file table.
struct FileSqlColumn
{
  FileSqlColumn()
    : name_(), type_(ObNullType), is_bool_(false), nullable_(false), max_len_(0), id_(0) {}
  common::ObString name_;
  ObObjType type_;
  bool is_bool_;      // BOOLEAN column (mapped to tinyint internally)
  bool nullable_;
  int64_t max_len_;   // max byte length for string columns
  int64_t id_;
};

// One cell produced by a reader.  The variant that is meaningful depends on
// the column type recorded in the schema: integer-ish types (including
// BOOLEAN/DATE/DATETIME) use int_val_, DOUBLE uses dbl_val_, string types use
// str_val_.
struct FileSqlValue
{
  FileSqlValue()
    : is_null_(true), int_val_(0), dbl_val_(0.0), str_val_() {}
  void set_null() { is_null_ = true; }
  bool is_null_;
  int64_t int_val_;
  double dbl_val_;
  common::ObString str_val_;
};

struct FileSqlCsvOptions
{
  FileSqlCsvOptions()
    : delimiter_(','), has_header_(false), sample_size_(-1), skip_header_lines_(0) {}
  char delimiter_;
  bool has_header_;
  int64_t sample_size_;      // -1 means sample the whole file
  int64_t skip_header_lines_;
};

// Infer the schema of a file (or of a glob pattern).
int infer_file_sql_schema(const common::ObString &path,
                          const int64_t format,
                          const FileSqlCsvOptions &csv_options,
                          common::ObIAllocator &alloc,
                          common::ObIArray<FileSqlColumn> &columns);

// Validate that a path is safe to read: no path escape, and if
// secure_file_priv is configured the path must live below it.
// Returns OB_SUCCESS and sets allowed accordingly.
int check_file_sql_path(const common::ObString &secure_file_priv,
                        const common::ObString &path,
                        bool &allowed);

// Streaming reader over a file table.
class FileSqlReader
{
public:
  FileSqlReader();
  ~FileSqlReader();

  // All strings handed out by next_row() live in the per-row allocator.
  int init(const common::ObString &path,
           const int64_t format,
           const FileSqlCsvOptions &csv_options,
           const common::ObIArray<FileSqlColumn> &columns,
           common::ObIAllocator &alloc);
  // Fill vals with one row; returns OB_ITER_END when the file is exhausted.
  int next_row(common::ObIAllocator &row_alloc, common::ObIArray<FileSqlValue> &vals);
  void reset();

private:
  int init_csv();
  int init_json();
  int init_parquet();
  int init_glob();
  int csv_next_row(common::ObIAllocator &row_alloc, common::ObIArray<FileSqlValue> &vals);
  int json_next_row(common::ObIAllocator &row_alloc, common::ObIArray<FileSqlValue> &vals);
  int parquet_next_row(common::ObIAllocator &row_alloc, common::ObIArray<FileSqlValue> &vals);
  int glob_next_row(common::ObIAllocator &row_alloc, common::ObIArray<FileSqlValue> &vals);
  int csv_read_record(common::ObIArray<common::ObString> &fields,
                      common::ObIArray<bool> &quoted,
                      common::ObIAllocator &alloc);
  int json_read_line(common::ObString &line, common::ObIAllocator &alloc);

  struct CsvState;
  struct JsonState;
  struct ParquetState;
  struct GlobState;

  bool inited_;
  int64_t format_;
  FileSqlCsvOptions csv_options_;
  common::ObString path_;
  common::ObArenaAllocator local_alloc_;
  CsvState *csv_;
  JsonState *json_;
  ParquetState *parquet_;
  GlobState *glob_;
  const common::ObIArray<FileSqlColumn> *columns_;
};

// Convert a FileSqlValue to a datum of the given target object type.
int file_sql_value_to_datum(const FileSqlValue &value,
                            const ObObjType target_type,
                            const common::ObCollationType cs_type,
                            common::ObIAllocator &alloc,
                            common::ObDatum &datum);

// Map a column type to a readable type name (used by DESCRIBE SELECT).
const char *file_sql_type_name(const ObObjType type, const bool is_bool);

} // namespace filesql
} // namespace sql
} // namespace oceanbase

#endif // OCEANBASE_SQL_OB_FILE_SQL_UTIL_H_
