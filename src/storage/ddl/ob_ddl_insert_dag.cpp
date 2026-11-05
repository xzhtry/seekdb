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

#include "data_plane/ddl/ob_ddl_schedule.h"
#include "storage/ddl/ob_ddl_insert_dag.h"
#include "storage/ddl/ob_ddl_tablet_context.h"
#include "storage/ddl/ob_tablet_slice_row_iterator.h"
#include "storage/ddl/ob_ddl_macro_block_write_task.h"
#include "storage/ddl/ob_ddl_direct_load_utils.h"
#include "storage/tx_storage/ob_ls_service.h"
#include "storage/ddl/ob_ddl_merge_task_v2.h"
#include "data_plane/fts/ob_fts_parser_helper.h"

#include <algorithm>
#include <vector>

#define USING_LOG_PREFIX STORAGE

using namespace oceanbase;
using namespace oceanbase::storage;
using namespace oceanbase::share;
using namespace oceanbase::share::schema;

ObFtsRangeBuildTask::ObFtsRangeBuildTask()
  : share::ObITask(share::ObITask::TASK_TYPE_DDL_WRITE_PIPELINE),
    is_inited_(false), tablet_id_(), slice_idx_(0), slice_count_(0)
{
}

int ObFtsRangeBuildTask::init(const common::ObTabletID &tablet_id,
                              const common::ObStoreRange &range,
                              const int64_t slice_idx,
                              const int64_t slice_count)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
  } else if (OB_UNLIKELY(!tablet_id.is_valid() || slice_idx < 0 || slice_count <= 0 || slice_idx >= slice_count)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    UNUSED(range);
    tablet_id_ = tablet_id;
    slice_idx_ = slice_idx;
    slice_count_ = slice_count;
    is_inited_ = true;
  }
  return ret;
}

int ObFtsRangeBuildTask::tokenize_text(const common::ObString &text,
                                        const common::ObString &parser_name,
                                        const common::ObString &properties,
                                        common::ObIAllocator &alloc,
                                        storage::ObFTWordMap &word_map,
                                        int64_t &doc_len)
{
  int ret = OB_SUCCESS;
  storage::ObFTParseHelper helper;
  common::ObObjMeta meta;
  meta.set_varchar();
  const int64_t bkt_cnt = MIN(MAX(text.length() / 2, 2), 997);
  if (OB_FAIL(helper.init(&alloc, parser_name, properties))) {
  } else if (OB_FAIL(word_map.create(bkt_cnt, common::ObMemAttr("FtsRangeWordMap")))) {
  } else if (0 != text.length() && OB_FAIL(helper.segment(meta, text.ptr(), text.length(), doc_len, word_map))) {
  }
  return ret;
}

int ObFtsRangeBuildTask::build_word_rows(const storage::ObFTWordMap &word_map,
                                         const int64_t doc_id,
                                         const int64_t doc_len,
                                         common::ObIAllocator &alloc,
                                         common::ObIArray<FtsAuxRow> &rows)
{
  int ret = OB_SUCCESS;
  rows.reuse();
  for (storage::ObFTWordMap::const_iterator it = word_map.begin();
       OB_SUCC(ret) && it != word_map.end(); ++it) {
    const common::ObDatum &word_datum = it->first.get_word();
    const common::ObString &word = word_datum.get_string();
    char *buf = nullptr;
    if (word.length() > 0 && OB_ISNULL(buf = static_cast<char *>(alloc.alloc(word.length())))) {
      ret = OB_ALLOCATE_MEMORY_FAILED;
      LOG_WARN("failed to allocate word buffer", K(ret), K(word.length()));
    } else {
      if (word.length() > 0) {
        MEMCPY(buf, word.ptr(), word.length());
      }
      FtsAuxRow row;
      row.word_.assign_ptr(buf, word.length());
      row.doc_id_ = doc_id;
      row.word_count_ = it->second;
      row.doc_length_ = doc_len;
      if (OB_FAIL(rows.push_back(row))) {
        LOG_WARN("failed to push aux row", K(ret));
      }
    }
  }
  return ret;
}

int ObFtsRangeBuildTask::sort_rows(common::ObIArray<FtsAuxRow> &rows)
{
  int ret = OB_SUCCESS;
  std::vector<FtsAuxRow> sorted;
  sorted.reserve(rows.count());
  // reserve() above makes these push_back calls allocation-free.
  for (int64_t i = 0; i < rows.count(); ++i) {
    sorted.push_back(rows.at(i));
  }
  if (OB_SUCC(ret)) {
    std::sort(sorted.begin(), sorted.end(), [](const FtsAuxRow &l, const FtsAuxRow &r) {
      const int cmp = l.word_.compare(r.word_);
      return cmp < 0 ? true : (cmp > 0 ? false : l.doc_id_ < r.doc_id_);
    });
    for (int64_t i = 0; OB_SUCC(ret) && i < rows.count(); ++i) {
      rows.at(i) = sorted.at(static_cast<size_t>(i));
    }
  }
  return ret;
}

int ObFtsRangeBuildTask::process()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
  } else {
    // Placeholder: the data path (scan this range -> tokenize -> sort -> write the
    // slice) is not implemented yet. Keep the task observable in the DDL log.
    LOG_INFO("fts range build task start",
             "tablet_id", tablet_id_, "slice_idx", slice_idx_, "slice_count", slice_count_);
    if (OB_FAIL(dag_yield())) {
    }
  }
  return ret;
}

ObDDLInsertDag::ObDDLInsertDag()
  : px_thread_count_(0), px_finished_count_(0), is_range_count_ready_(false), total_slice_count_(0), use_static_plan_(true)
{

}

ObDDLInsertDag::~ObDDLInsertDag()
{

}

int ObDDLInsertDag::init_by_param(const share::ObIDagInitParam *param)
{
  int ret = OB_SUCCESS;
  ObITask *merge_parent_task = nullptr;
  const ObDDLInsertDagInitParam *init_param = static_cast<const ObDDLInsertDagInitParam*>(param);
  if (OB_UNLIKELY(nullptr == init_param || !init_param->is_valid())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(ObDDLIndependentDag::init_by_param(init_param))) {
  } else {
    px_thread_count_ = init_param->px_thread_count_;
    is_inited_ = true;

    ObArray<ObITask *> write_macro_block_tasks;
    if (OB_FAIL(generate_write_macro_block_tasks(write_macro_block_tasks))) {
    } else if (OB_FAIL(batch_add_task(write_macro_block_tasks))) {
    }
  }
  FLOG_INFO("ddl insert dag init", K(ret), KPC(this));
  return ret;
}

int ObDDLInsertDag::set_px_finished()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
  } else {
    ATOMIC_INC(&px_finished_count_);
    if (is_scan_finished() && !use_static_plan_) {
      /* do nothing */
    }
  }
  FLOG_INFO("set px finished", K(px_finished_count_), K(px_thread_count_));
  return ret;
}

int ObDDLInsertDag::update_tablet_range_count()
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
  } else {
    ObMutexGuard mutex_guard(mutex_);
    if (is_range_count_ready_) {
      // do nothing
    } else {
      ObArray<data_plane::ObDDLTabletSliceCount> tablet_slice_counts;
      if (OB_FAIL(data_plane::load_idempotent_ddl_tablet_slice_counts(
              ddl_task_param_.ddl_task_id_, tablet_slice_counts))) {
      } else {
        total_slice_count_ = 0;
        if (0 == tablet_slice_counts.count()) {
          ret = OB_ERR_UNEXPECTED;
        } else if (1 == tablet_slice_counts.count() && 0 == tablet_slice_counts.at(0).tablet_id_) {
          // for unpartitioned table, there is only one tablet and its tablet id is 0
          if (tablet_ids_.count() != 1) {
            ret = OB_ERR_UNEXPECTED;
            LOG_WARN("tablet count not match", K(ret), K(tablet_slice_counts.count()), K(tablet_ids_));
          } else {
            const ObTabletID &tablet_id = tablet_ids_.at(0);
            total_slice_count_ = tablet_slice_counts.at(0).slice_count_;
            ObDDLTabletContext *tablet_context = nullptr;
            if (OB_FAIL(get_tablet_context(tablet_id, tablet_context))) {
            } else {
              tablet_context->slice_count_ = total_slice_count_;
              tablet_context->table_slice_offset_ = 0;
            }
          }
        } else {
          for (int64_t i = 0; OB_SUCC(ret) && i < tablet_slice_counts.count(); ++i) {
            const data_plane::ObDDLTabletSliceCount &slice_count = tablet_slice_counts.at(i);
            const int64_t tablet_slice_count = slice_count.slice_count_;
            ObTabletID tablet_id(slice_count.tablet_id_);
            ObDDLTabletContext *tablet_context = nullptr;
            if (OB_FAIL(get_tablet_context(tablet_id, tablet_context))) {
              if (OB_HASH_NOT_EXIST != ret) {
              } else {
                // may get tablet not in this node, skip it, but add total slice count
                total_slice_count_ += tablet_slice_count;
                ret = OB_SUCCESS;
              }
            } else {
              tablet_context->slice_count_ = tablet_slice_count;
              tablet_context->table_slice_offset_ = total_slice_count_;
              total_slice_count_ += tablet_slice_count;
            }
          }
        }
      }
      if (OB_SUCC(ret)) {
        is_range_count_ready_ = true;
      }
    }
  }
  return ret;
}
