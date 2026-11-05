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
#include "storage/ddl/ob_complement_data_task.h"
#include "storage/ddl/ob_pipeline.h"
#include "storage/ddl/ob_ddl_batch_datum_rows.h"

#include <algorithm>
#include <vector>

#define USING_LOG_PREFIX STORAGE

using namespace oceanbase;
using namespace oceanbase::storage;
using namespace oceanbase::share;
using namespace oceanbase::share::schema;

ObFtsRangeBuildTask::ObFtsRangeBuildTask()
  : share::ObITask(share::ObITask::TASK_TYPE_DDL_WRITE_PIPELINE),
    is_inited_(false), dry_run_(false), dag_(nullptr), tablet_id_(), slice_idx_(0), slice_count_(0),
    table_schema_alloc_(lib::ObMemAttr("FtsRangeBatch"))
{
}

int ObFtsRangeBuildTask::init(ObDDLIndependentDag *dag,
                              const common::ObTabletID &tablet_id,
                              const int64_t slice_idx,
                              const int64_t slice_count,
                              const bool dry_run)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
  } else if (OB_UNLIKELY(OB_ISNULL(dag) || !tablet_id.is_valid() ||
                         slice_idx < 0 || slice_count <= 0 || slice_idx >= slice_count)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    dag_ = dag;
    dry_run_ = dry_run;
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

int ObFtsRangeBuildTask::write_slice(ObDDLIndependentDag *dag,
                                     const common::ObIArray<FtsAuxRow> &rows)
{
  int ret = OB_SUCCESS;
  ObDDLTabletContext *tablet_context = nullptr;
  storage::ObWriteMacroParam param;
  storage::ObDDLBatchDatumRows *batch = nullptr;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
  } else if (OB_ISNULL(dag) || rows.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(dag->get_tablet_context(tablet_id_, tablet_context))) {
  } else if (OB_ISNULL(tablet_context)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    const ObDDLTaskParam &task_param = dag->get_ddl_task_param();
    param.tablet_id_ = tablet_id_;
    param.data_format_version_ = task_param.data_format_version_;
    param.schema_version_ = task_param.schema_version_;
    param.slice_idx_ = slice_idx_;
    param.slice_count_ = slice_count_;
    param.ddl_thread_count_ = slice_count_;
    param.snapshot_version_ = task_param.snapshot_version_;
    param.direct_load_type_ = storage::ObDDLDirectLoadUtil::ddl_get_direct_load_type();
    param.task_id_ = task_param.ddl_task_id_;
    param.is_index_table_ = true;
    param.ddl_table_schema_ = dag->get_ddl_table_schema();
    param.tablet_param_ = tablet_context->tablet_param_;
    param.ddl_dag_ = dag;
    param.tablet_context_ = tablet_context;

    storage::ObPipeline pipeline(share::ObITask::TASK_TYPE_DDL_WRITE_PIPELINE);
    storage::ObDDLWriteMacroBlockOperator write_op(&pipeline);
    if (OB_FAIL(write_op.init(param))) {
      LOG_WARN("failed to init ddl write macro block operator", K(ret));
    } else if (OB_FAIL(pipeline.add_op(&write_op))) {
    } else if (OB_FAIL(build_batch(dag, rows, rows.count(), batch))) {
    } else {
      storage::ObChunk batch_chunk;
      batch_chunk.type_ = storage::ObChunk::DDL_BATCH_DATUM_ROWS;
      batch_chunk.ddl_batch_rows_ = batch;
      if (OB_FAIL(pipeline.push(batch_chunk))) {
        LOG_WARN("failed to push aux batch", K(ret));
      } else {
        storage::ObChunk end_chunk;
        end_chunk.set_end_chunk();
        if (OB_FAIL(pipeline.push(end_chunk))) {
          LOG_WARN("failed to close slice writer", K(ret));
        }
      }
    }
  }
  return ret;
}

int ObFtsRangeBuildTask::build_batch(ObDDLIndependentDag *dag,
                                     const common::ObIArray<FtsAuxRow> &rows,
                                     const int64_t max_batch_size,
                                     storage::ObDDLBatchDatumRows *&batch)
{
  int ret = OB_SUCCESS;
  batch = nullptr;
  const storage::ObDDLTableSchema &table_schema = dag->get_ddl_table_schema();
  if (OB_UNLIKELY(rows.count() <= 0 || max_batch_size <= 0)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_UNLIKELY(table_schema.column_items_.count() <= 0)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_ISNULL(batch = OB_NEWx(storage::ObDDLBatchDatumRows, &table_schema_alloc_))) {
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    storage::ObDDLRowFlag row_flag;
    const int64_t batch_size = MIN(max_batch_size, rows.count());
    if (OB_FAIL(batch->batch_rows_.init(table_schema.column_items_, batch_size, row_flag))) {
      LOG_WARN("failed to init ddl batch rows", K(ret));
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < batch_size; ++i) {
        const FtsAuxRow &row = rows.at(i);
        blocksstable::ObStorageDatum datums[4];
        datums[0].set_string(row.word_);
        datums[1].set_int(row.doc_id_);
        datums[2].set_int(row.word_count_);
        datums[3].set_int(row.doc_length_);
        if (OB_FAIL(batch->batch_rows_.append_row(datums, 4))) {
          LOG_WARN("failed to append aux row", K(ret));
        }
      }
    }
    if (OB_SUCC(ret)) {
      batch->datum_rows_.reset();
      for (int64_t i = 0; OB_SUCC(ret) && i < batch->batch_rows_.get_vectors().count(); ++i) {
        storage::ObDDLVector *ddl_vector = batch->batch_rows_.get_vectors().at(i);
        blocksstable::ObIVector *vector = nullptr;
        if (OB_ISNULL(ddl_vector) || OB_ISNULL(vector = ddl_vector->get_vector())) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(batch->datum_rows_.vectors_.push_back(vector))) {
          LOG_WARN("failed to link vector", K(ret));
        }
      }
      if (OB_SUCC(ret)) {
        batch->datum_rows_.row_count_ = batch->batch_rows_.size();
      }
    }
  }
  return ret;
}

int ObFtsRangeBuildTask::write_slice(ObDDLIndependentDag *dag)
{
  int ret = OB_SUCCESS;
  ObDDLTabletContext *tablet_context = nullptr;
  storage::ObWriteMacroParam param;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
  } else if (OB_ISNULL(dag)) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(dag->get_tablet_context(tablet_id_, tablet_context))) {
  } else if (OB_ISNULL(tablet_context)) {
    ret = OB_ERR_UNEXPECTED;
  } else {
    const ObDDLTaskParam &task_param = dag->get_ddl_task_param();
    param.tablet_id_ = tablet_id_;
    param.data_format_version_ = task_param.data_format_version_;
    param.schema_version_ = task_param.schema_version_;
    param.slice_idx_ = slice_idx_;
    param.slice_count_ = slice_count_;
    param.ddl_thread_count_ = slice_count_;
    param.snapshot_version_ = task_param.snapshot_version_;
    param.direct_load_type_ = storage::ObDDLDirectLoadUtil::ddl_get_direct_load_type();
    param.task_id_ = task_param.ddl_task_id_;
    param.is_index_table_ = true;
    param.ddl_table_schema_ = dag->get_ddl_table_schema();
    param.tablet_param_ = tablet_context->tablet_param_;
    param.ddl_dag_ = dag;
    param.tablet_context_ = tablet_context;

    storage::ObPipeline pipeline(share::ObITask::TASK_TYPE_DDL_WRITE_PIPELINE);
    storage::ObDDLWriteMacroBlockOperator write_op(&pipeline);
    if (OB_FAIL(write_op.init(param))) {
      LOG_WARN("failed to init ddl write macro block operator", K(ret));
    } else if (OB_FAIL(pipeline.add_op(&write_op))) {
    } else {
      storage::ObChunk end_chunk;
      end_chunk.set_end_chunk();
      if (OB_FAIL(pipeline.push(end_chunk))) {
        LOG_WARN("failed to close slice writer", K(ret));
      }
    }
  }
  return ret;
}

int ObFtsRangeBuildTask::scan_range(const share::schema::ObTableSchema &data_schema,
                                    const share::schema::ObTableSchema &aux_schema,
                                    const common::ObString &parser_name,
                                    const common::ObString &parser_properties,
                                    const int64_t snapshot_version,
                                    blocksstable::ObDatumRange &range,
                                    common::ObIAllocator &alloc,
                                    common::ObIArray<FtsAuxRow> &out_rows,
                                    int64_t &row_cnt,
                                    int64_t &token_cnt)
{
  int ret = OB_SUCCESS;
  row_cnt = 0;
  token_cnt = 0;
  int64_t text_col_idx = -1;
  int64_t doc_id_col_idx = -1;
  common::ObSEArray<share::schema::ObColDesc, 16> col_ids;
  common::ObSEArray<int32_t, 16> projector;
  storage::ObLocalScan local_scan;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(data_schema.get_column_ids(col_ids))) {
  } else {
    for (int64_t i = 0; OB_SUCC(ret) && i < col_ids.count(); ++i) {
      const share::schema::ObColumnSchemaV2 *column_schema = nullptr;
      if (OB_FAIL(projector.push_back(static_cast<int32_t>(i)))) {
      } else if (OB_ISNULL(column_schema = data_schema.get_column_schema(col_ids.at(i).col_id_))) {
        ret = OB_ERR_UNEXPECTED;
      } else if (column_schema->is_word_segment_column()) {
        text_col_idx = i;
      } else if (column_schema->is_doc_id_column()) {
        doc_id_col_idx = i;
      }
    }
  }

  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(local_scan.init(col_ids, col_ids, projector, data_schema,
                                     snapshot_version, aux_schema, false /*unique_index_checking*/))) {
  } else {
    storage::ObLS *ls = nullptr;
    ObTabletTableIterator table_iter;
    const bool allow_not_ready = false;
    if (OB_FAIL(share::server_service<storage::ObLSService>()->get_ls(ls))) {
    } else if (OB_ISNULL(ls)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(ls->get_tablet_svr()->get_read_tables(
                   tablet_id_, ObTabletCommon::DEFAULT_GET_TABLET_DURATION_US,
                   snapshot_version, snapshot_version, table_iter, allow_not_ready))) {
      if (OB_REPLICA_NOT_READABLE == ret) {
        ret = OB_EAGAIN;
      }
    } else {
      ObQueryFlag query_flag(ObQueryFlag::Forward, false, true, false, false, false, false);
      query_flag.disable_cache();
      query_flag.skip_read_lob_ = 1;
      if (OB_FAIL(local_scan.table_scan(data_schema, tablet_id_, table_iter, query_flag, range))) {
      } else if (OB_UNLIKELY(text_col_idx < 0 || doc_id_col_idx < 0)) {
        ret = OB_ERR_UNEXPECTED;
        LOG_WARN("fts aux build: word segment / doc id column not found",
                 K(ret), K(text_col_idx), K(doc_id_col_idx));
      } else {
        const blocksstable::ObDatumRow *row = nullptr;
        common::ObSEArray<FtsAuxRow, 1024> doc_rows;
        storage::ObFTWordMap word_map;
        while (OB_SUCC(local_scan.get_next_row(row))) {
          int64_t doc_len = 0;
          if (OB_ISNULL(row)) {
            ret = OB_ERR_UNEXPECTED;
          } else if (OB_FAIL(tokenize_text(row->storage_datums_[text_col_idx].get_string(),
                                           parser_name, parser_properties,
                                           alloc, word_map, doc_len))) {
          } else if (FALSE_IT(doc_rows.reuse())) {
          } else if (OB_FAIL(build_word_rows(word_map, row->storage_datums_[doc_id_col_idx].get_int(),
                                             doc_len, alloc, doc_rows))) {
          } else if (OB_FAIL(out_rows.assign(doc_rows))) {
          } else {
            ++row_cnt;
            token_cnt += doc_rows.count();
          }
        }
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        }
      }
    }
  }
  return ret;
}

int ObFtsRangeBuildTask::process()
{
  int ret = OB_SUCCESS;
  share::schema::ObSchemaGetterGuard schema_guard;
  const share::schema::ObTableSchema *aux_schema = nullptr;
  const share::schema::ObTableSchema *data_schema = nullptr;
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
    LOG_WARN("fts range build task not inited", K(ret));
  } else if (OB_ISNULL(dag_)) {
    ret = OB_INVALID_ARGUMENT;
    LOG_WARN("fts range build task without dag", K(ret));
  } else if (OB_ISNULL(GCTX.schema_service_)) {
    ret = OB_ERR_UNEXPECTED;
    LOG_WARN("schema service is null", K(ret));
  } else {
    const uint64_t aux_table_id = dag_->get_ddl_task_param().target_table_id_;
    LOG_INFO("fts range build task start", K(aux_table_id), K_(slice_idx), K_(slice_count));
    if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(schema_guard))) {
      LOG_WARN("failed to get schema guard", K(ret));
    } else if (OB_FAIL(schema_guard.get_table_schema(aux_table_id, aux_schema))) {
      LOG_WARN("failed to get aux table schema", K(ret), K(aux_table_id));
    } else if (OB_ISNULL(aux_schema)) {
      ret = OB_TABLE_NOT_EXIST;
      LOG_WARN("aux table schema not found", K(ret), K(aux_table_id));
    } else if (OB_FAIL(schema_guard.get_table_schema(aux_schema->get_data_table_id(), data_schema))) {
    } else if (OB_ISNULL(data_schema)) {
      ret = OB_TABLE_NOT_EXIST;
    } else {
      common::ObArenaAllocator alloc(lib::ObMemAttr("FtsRangeTask"));
      common::ObSEArray<FtsAuxRow, 1024> rows;
      blocksstable::ObDatumRange whole_range;
      whole_range.set_whole_range();
      int64_t row_cnt = 0;
      int64_t token_cnt = 0;
      if (OB_FAIL(scan_range(*data_schema, *aux_schema,
                             aux_schema->get_parser_name_str(),
                             aux_schema->get_parser_property_str(),
                             dag_->get_ddl_task_param().snapshot_version_,
                             whole_range, alloc, rows, row_cnt, token_cnt))) {
        LOG_WARN("failed to scan/build rows", K(ret));
      } else if (OB_FAIL(sort_rows(rows))) {
      } else if (dry_run_) {
        LOG_INFO("fts range build dry run", K(ret), K(row_cnt), K(token_cnt), K(rows.count()),
                 K_(slice_idx), K_(slice_count));
      } else if (OB_FAIL(write_slice(dag_, rows))) {
      } else {
        LOG_INFO("fts range build done", K(ret), K(row_cnt), K(token_cnt), K(rows.count()),
                 K_(slice_idx), K_(slice_count));
      }
    }
  }
  return ret;
}

// Experimental switch: schedule the per-range FTS builder (dry run) during DDL insert.
static constexpr bool ENABLE_FTS_RANGE_DRYRUN = false;

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
    } else if (OB_SUCC(ret) && ENABLE_FTS_RANGE_DRYRUN) {
      // Experimental: run the per-range FTS row builder alongside the normal build to
      // validate scan/tokenize/sort numbers. Scheduling problems must not fail the DDL.
      int tmp_ret = OB_SUCCESS;
      ObArray<ObITask *> extra_tasks;
      ObITask *scan_task = write_macro_block_tasks.empty() ? nullptr : write_macro_block_tasks.at(0);
      LOG_INFO("fts dryrun: scheduling range tasks", "tablets", tablet_ids_.count(),
               "tasks", write_macro_block_tasks.count());
      UNUSED(scan_task);
      for (int64_t i = 0; OB_SUCC(tmp_ret) && i < tablet_ids_.count(); ++i) {
        ObFtsRangeBuildTask *range_task = nullptr;
        if (OB_FAIL(alloc_task(range_task))) {
        } else if (OB_ISNULL(range_task)) {
          tmp_ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(range_task->init(this, tablet_ids_.at(i), 0, 1, true /*dry_run*/))) {
        } else if (OB_FAIL(write_macro_block_tasks.at(write_macro_block_tasks.count() - 1)->add_child(*range_task))) {
        } else if (OB_FAIL(extra_tasks.push_back(range_task))) {
        }
      }
      if (OB_FAIL(tmp_ret)) {
        LOG_WARN("fts dryrun: failed to build range tasks", K(tmp_ret));
      } else if (OB_FAIL(tmp_ret = batch_add_task(extra_tasks))) {
        LOG_WARN("fts dryrun: failed to add range tasks", K(tmp_ret));
      } else {
        LOG_INFO("fts dryrun: scheduled", "tasks", extra_tasks.count());
      }
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
