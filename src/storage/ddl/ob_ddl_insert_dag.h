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
  int init(const common::ObTabletID &tablet_id,
           const common::ObStoreRange &range,
           const int64_t slice_idx,
           const int64_t slice_count);
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
private:
  bool is_inited_;
  common::ObTabletID tablet_id_;
  int64_t slice_idx_;
  int64_t slice_count_;
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
