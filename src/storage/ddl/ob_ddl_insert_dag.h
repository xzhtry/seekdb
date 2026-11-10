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

#ifndef _STORAGE_DDL_OB_DDL_INSERT_DAG_
#define _STORAGE_DDL_OB_DDL_INSERT_DAG_

#include "storage/ddl/ob_ddl_struct.h"
#include "storage/ddl/ob_ddl_independent_dag.h"
#include "data_plane/scheduler/ob_dag_scheduler.h"
#include "data_plane/fts/ob_fts_struct.h"

namespace oceanbase
{

namespace storage
{

struct ObDDLInsertDagInitParam : public ObDDLIndependentDagInitParam
{
public:
  ObDDLInsertDagInitParam() : px_thread_count_(0) {}
  virtual bool is_valid() const override { return ObDDLIndependentDagInitParam::is_valid() && px_thread_count_ > 0; }
  INHERIT_TO_STRING_KV("DDLDagInitParm", ObDDLIndependentDagInitParam, K(px_thread_count_));

public:
  int64_t px_thread_count_;
};

// Parameters for building one FTS inverted-index auxiliary table straight from a
// base-table scan instead of driving an inner `INSERT ... SELECT ... ORDER BY`.
struct ObFtsAuxBuildParam final
{
public:
  ObFtsAuxBuildParam()
    : data_table_id_(OB_INVALID_ID), aux_table_id_(OB_INVALID_ID), schema_version_(0),
      snapshot_version_(0), ddl_task_id_(0), execution_id_(0), parallelism_(1)
  {}
  bool is_valid() const
  {
    return OB_INVALID_ID != data_table_id_ && OB_INVALID_ID != aux_table_id_ &&
           schema_version_ > 0 && snapshot_version_ > 0 &&
           ddl_task_id_ > 0 && execution_id_ >= 0 && parallelism_ > 0;
  }
  TO_STRING_KV(K_(data_table_id), K_(aux_table_id), K_(schema_version), K_(snapshot_version),
               K_(ddl_task_id), K_(execution_id), K_(parallelism));
public:
  uint64_t data_table_id_;
  uint64_t aux_table_id_;
  int64_t schema_version_;
  int64_t snapshot_version_;
  int64_t ddl_task_id_;
  int64_t execution_id_;
  int64_t parallelism_;
};

// Build the inverted-index aux table by scanning the base table directly.
// Returns OB_NOT_SUPPORTED when the direct build is disabled or not applicable,
// in which case the caller must fall back to the inner SQL path.
int build_fts_inverted_aux_by_scan(const ObFtsAuxBuildParam &param);

class ObFtsRangeBuildTask final : public share::ObITask
{
public:
  struct FtsAuxRow
  {
    common::ObString word_;
    int64_t doc_id_;
    int64_t word_count_;
    int64_t doc_length_;
  };
  ObFtsRangeBuildTask();
  virtual ~ObFtsRangeBuildTask() = default;
  // One task per base-table block range; slice_idx identifies the slice it writes.
  int init(ObDDLIndependentDag *dag,
           const common::ObTabletID &tablet_id,
           const int64_t slice_idx,
           const int64_t slice_count,
           const bool dry_run = false);
  virtual int process() override;
  // Tokenize one document's fulltext into a word->count map (building block for the
  // per-range aux row generation).
  int tokenize_text(const common::ObString &text,
                    const common::ObString &parser_name,
                    const common::ObString &properties,
                    common::ObIAllocator &alloc,
                    storage::ObFTWordMap &word_map,
                    int64_t &doc_len);
  // Expand one document's word map into the aux-table rows (one row per distinct word).
  int build_word_rows(const storage::ObFTWordMap &word_map,
                      const int64_t doc_id,
                      const int64_t doc_len,
                      common::ObIAllocator &alloc,
                      common::ObIArray<FtsAuxRow> &rows);
  // Sort rows by (word, doc_id). TODO: use the aux column collation once the task
  // carries the schema; binary order is used for now.
  int sort_rows(common::ObIArray<FtsAuxRow> &rows);
  // Scan one base-table range through a local scan and count the rows (the row
  // payload processing is added incrementally).
  // Drive the DDL write pipeline for this task's slice (opens the slice writer and
  // closes it via an end chunk; row batches are added incrementally).
  int write_slice(ObDDLIndependentDag *dag);
  // Same as above, but writes the given (sorted) rows into this slice before closing.
  int write_slice(ObDDLIndependentDag *dag, const common::ObIArray<FtsAuxRow> &rows);
  // Convert sorted aux rows into a DDL batch (row order == aux column order:
  // word, doc id, word count, doc length).
  int build_batch(ObDDLIndependentDag *dag,
                  const common::ObIArray<FtsAuxRow> &rows,
                  const int64_t max_batch_size,
                  storage::ObDDLBatchDatumRows *&batch);
  int scan_range(const share::schema::ObTableSchema &data_schema,
                 const share::schema::ObTableSchema &aux_schema,
                 const common::ObString &parser_name,
                 const common::ObString &parser_properties,
                 const int64_t snapshot_version,
                 blocksstable::ObDatumRange &range,
                 common::ObIAllocator &alloc,
                 common::ObIArray<FtsAuxRow> &out_rows,
                 int64_t &row_cnt,
                 int64_t &token_cnt);
private:
  bool is_inited_;
  bool dry_run_;
  ObDDLIndependentDag *dag_;
  common::ObTabletID tablet_id_;
  int64_t slice_idx_;
  int64_t slice_count_;
  common::ObArenaAllocator table_schema_alloc_;
};

class ObDDLInsertDag : public ObDDLIndependentDag
{
public:
  ObDDLInsertDag();
  virtual ~ObDDLInsertDag();
  virtual int init_by_param(const share::ObIDagInitParam *param) override;

  int set_px_finished();
  int update_tablet_range_count();
  int64_t get_total_slice_count() const { return total_slice_count_; }
  virtual bool is_scan_finished() override { return px_thread_count_ > 0 && px_finished_count_ >= px_thread_count_; }

  INHERIT_TO_STRING_KV("DDLDag", ObDDLIndependentDag, K_(px_thread_count), K_(px_finished_count), K_(is_range_count_ready), K_(total_slice_count), K_(use_static_plan));

protected:
  int64_t px_thread_count_;
  int64_t px_finished_count_;
  lib::ObMutex mutex_;
  bool is_range_count_ready_; // update table total slice count and each tablet slice count
  int64_t total_slice_count_; // for idempotence of user autoinc column
  bool use_static_plan_;
};


}// namespace storage
}// namespace oceanbase

#endif//_STORAGE_DDL_OB_DDL_INSERT_DAG_
