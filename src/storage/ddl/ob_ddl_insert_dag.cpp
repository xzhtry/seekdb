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
#include "storage/ddl/ob_ddl_storage_util.h"
#include "storage/ddl/ob_tablet_slice_writer.h"
#include "storage/tx_storage/ob_ls_service.h"
#include "storage/ddl/ob_ddl_merge_task_v2.h"
#include "data_plane/fts/ob_fts_parser_helper.h"
#include "storage/ddl/ob_complement_data_task.h"
#include "storage/ddl/ob_pipeline.h"
#include "storage/ddl/ob_ddl_batch_datum_rows.h"
#include "lib/charset/ob_charset.h"
#include "share/ob_ddl_common.h"
#include "share/schema/ob_multi_version_schema_service.h"

#include <algorithm>
#include <cstring>
#include <functional>
#include <queue>
#include <thread>
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
  LOG_INFO("fts range scan: enter", K_(tablet_id), K(snapshot_version));
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
    const bool allow_not_ready = true;
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
      LOG_INFO("fts range scan: read tables ready", K_(tablet_id), K(snapshot_version));
      if (OB_FAIL(local_scan.table_scan(data_schema, tablet_id_, table_iter, query_flag, range))) {
        LOG_WARN("fts range scan: table_scan failed", K(ret));
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
          } else {
            for (int64_t i = 0; OB_SUCC(ret) && i < doc_rows.count(); ++i) {
              if (OB_FAIL(out_rows.push_back(doc_rows.at(i)))) {
              }
            }
            if (OB_SUCC(ret)) {
              ++row_cnt;
              token_cnt += doc_rows.count();
            }
          }
        }
        if (OB_ITER_END == ret) {
          ret = OB_SUCCESS;
        }
        LOG_INFO("fts range scan: finished", K(row_cnt), K(token_cnt));
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
    if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard_with_version_in_inner_table(schema_guard))) {
      LOG_WARN("failed to get ddl schema guard", K(ret));
    } else {
      LOG_INFO("fts range: guard ok", K(aux_table_id));
      if (OB_FAIL(schema_guard.get_table_schema(aux_table_id, aux_schema))) {
        LOG_WARN("failed to get aux table schema", K(ret), K(aux_table_id));
      } else if (OB_ISNULL(aux_schema)) {
        ret = OB_TABLE_NOT_EXIST;
        LOG_WARN("aux table schema not found", K(ret), K(aux_table_id));
      } else {
        const uint64_t data_table_id = aux_schema->get_data_table_id();
        LOG_INFO("fts range: aux schema ok", K(aux_table_id), K(data_table_id));
        if (OB_FAIL(schema_guard.get_table_schema(data_table_id, data_schema))) {
          LOG_WARN("failed to get data table schema", K(ret), K(data_table_id));
        } else if (OB_ISNULL(data_schema)) {
          ret = OB_TABLE_NOT_EXIST;
          LOG_WARN("data table schema not found", K(ret), K(data_table_id));
        } else {
          LOG_INFO("fts range: schemas ready", K(aux_table_id), K(data_table_id));
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


// ---------------------------------------------------------------------------
// FTS inverted-index aux build: row production straight from a base-table scan.
//
// The stock path runs an inner `INSERT ... SELECT ... ORDER BY word, doc_id`
// through the SQL engine, so every aux row pays for the plan: expression eval,
// PX transmit, a general collation-aware sort over four columns and the DAS
// insert layer.  The producer below scans the base table by primary key and
// builds the same (word, doc_id, word_count, doc_length) rows directly.
//
// Row semantics that must stay identical to the inner SQL:
//   * word       = tokens of the indexed columns joined by a single space
//                  (ObExprWordSegment::generate_fulltext_column)
//   * doc_id     = ObDocId(tablet_id, seq); under the idempotent autoinc scheme
//                  seq collapses to the scan-order row index for any slice
//                  layout (ObDDLUtil::generate_idempotent_value)
//   * word_count = occurrences of the word in the document
//   * doc_length = total token count of the document
//   * row order  = (word under the aux word-column collation, doc_id)
// ---------------------------------------------------------------------------
namespace
{

bool fts_aux_direct_enabled()
{
  // On by default; FTS_AUX_DIRECT=0 is the kill switch.
  static const bool enabled = []() {
    const char *v = getenv("FTS_AUX_DIRECT");
    return nullptr == v || '\0' == v[0] || '0' != v[0];
  }();
  return enabled;
}

bool fts_aux_dryrun_enabled()
{
  static const bool enabled = []() {
    const char *v = getenv("FTS_AUX_DRYRUN");
    return nullptr != v && '\0' != v[0] && '0' != v[0];
  }();
  return enabled;
}

struct ObFtsAuxRowRec
{
  const char *word_;
  int32_t word_len_;
  const char *doc_id_str_;  // 16-byte ObDocId, read from the data table
  int64_t doc_id_;
  uint64_t word_count_;
  uint64_t doc_length_;
};

class ObFtsAuxProducer
{
public:
  ObFtsAuxProducer()
    : alloc_(lib::ObMemAttr("FtsAuxBuild")),
      is_inited_(false),
      data_schema_(nullptr),
      aux_schema_(nullptr),
      word_collation_(CS_TYPE_INVALID),
      text_collation_(CS_TYPE_INVALID),
      snapshot_version_(0),
      doc_id_col_id_(OB_INVALID_ID)
  {}
  ~ObFtsAuxProducer() = default;
  int init(const share::schema::ObTableSchema &data_schema,
           const share::schema::ObTableSchema &aux_schema,
           const int64_t snapshot_version);
  int build_tablet(const common::ObTabletID &tablet_id,
                   common::ObIArray<ObFtsAuxRowRec> &rows,
                   int64_t &doc_cnt,
                   int64_t &token_cnt);
  int build_tablet_parallel(const int64_t slice_cnt,
                            common::ObIArray<ObFtsAuxRowRec> &rows,
                            int64_t &doc_cnt,
                            int64_t &token_cnt);
  ObCollationType get_word_collation() const { return word_collation_; }
  ObCollationType get_text_collation() const { return text_collation_; }
  const common::ObIArray<uint64_t> &get_text_col_ids() const { return text_col_ids_; }
  const common::ObString &get_table_name() const { return data_table_name_; }
  const common::ObString &get_parser_name() const { return aux_schema_->get_parser_name_str(); }
  const common::ObString &get_parser_props() const { return aux_schema_->get_parser_property_str(); }
  int64_t get_snapshot_version() const { return snapshot_version_; }
  void set_data_table_name(const common::ObString &name)
  {
    MEMSET(table_name_buf_, 0, sizeof(table_name_buf_));
    if (!name.empty() && name.length() < static_cast<int32_t>(sizeof(table_name_buf_))) {
      MEMCPY(table_name_buf_, name.ptr(), name.length());
      data_table_name_.assign_ptr(table_name_buf_, name.length());
    }
  }

private:
  int resolve_columns();
  int tokenize_text(const common::ObString &text, storage::ObFTWordMap &word_map, int64_t &doc_length);
  int append_select_columns(common::ObSqlString &sql) const;
  int read_key_bounds(int64_t &min_key, int64_t &max_key);

private:
  common::ObArenaAllocator alloc_;
  bool is_inited_;
  const share::schema::ObTableSchema *data_schema_;
  const share::schema::ObTableSchema *aux_schema_;
  common::ObSEArray<uint64_t, 4> text_col_ids_;
  char table_name_buf_[OB_MAX_USER_TABLE_NAME_LENGTH_MYSQL + 1];
  common::ObString data_table_name_;
  common::ObSEArray<int64_t, 4> text_col_pos_; // position of each indexed column in the scan output
  ObCollationType word_collation_;
  ObCollationType text_collation_;
  int64_t snapshot_version_;
  uint64_t doc_id_col_id_;
  storage::ObFTParseHelper helper_;
  common::ObSEArray<common::ObArenaAllocator *, 8> slice_allocs_;
};

int ObFtsAuxProducer::init(const share::schema::ObTableSchema &data_schema,
                           const share::schema::ObTableSchema &aux_schema,
                           const int64_t snapshot_version)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(is_inited_)) {
    ret = OB_INIT_TWICE;
  } else if (OB_UNLIKELY(snapshot_version <= 0)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    data_schema_ = &data_schema;
    aux_schema_ = &aux_schema;
    snapshot_version_ = snapshot_version;
    if (OB_FAIL(resolve_columns())) {
      LOG_WARN("fts aux producer resolve columns failed", K(ret));
    } else if (OB_FAIL(helper_.init(&alloc_,
                                    aux_schema.get_parser_name_str(),
                                    aux_schema.get_parser_property_str()))) {
      LOG_WARN("fts aux producer parse helper init failed", K(ret),
               K(aux_schema.get_parser_name_str()), K(aux_schema.get_parser_property_str()));
    } else {
      is_inited_ = true;
    }
  }
  return ret;
}

int ObFtsAuxProducer::resolve_columns()
{
  int ret = OB_SUCCESS;
  uint64_t word_col_id = OB_INVALID_ID;
  common::ObSEArray<share::schema::ObColDesc, 16> scan_cols;
  if (OB_ISNULL(data_schema_) || OB_ISNULL(aux_schema_)) {
    ret = OB_NOT_INIT;
  } else {
    for (share::schema::ObTableSchema::const_column_iterator iter = aux_schema_->column_begin();
         OB_SUCC(ret) && iter != aux_schema_->column_end(); ++iter) {
      const share::schema::ObColumnSchemaV2 *col = *iter;
      if (OB_ISNULL(col)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (word_col_id == OB_INVALID_ID && col->is_word_segment_column()) {
        word_col_id = col->get_column_id();
        word_collation_ = col->get_collation_type();
      }
    }
    const share::schema::ObColumnSchemaV2 *data_word_col = nullptr;
    if (OB_FAIL(ret)) {
    } else if (OB_UNLIKELY(OB_INVALID_ID == word_col_id)) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("fts aux word column not found", K(ret), KPC(aux_schema_));
    } else if (OB_ISNULL(data_word_col = data_schema_->get_column_schema(word_col_id))) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("data word segment column not found", K(ret), K(word_col_id));
    } else if (OB_FAIL(data_word_col->get_cascaded_column_ids(text_col_ids_))) {
    } else if (OB_UNLIKELY(text_col_ids_.empty())) {
      ret = OB_ERR_UNEXPECTED;
      LOG_WARN("fts indexed columns empty", K(ret), K(word_col_id));
    } else if (OB_FAIL(data_schema_->get_column_ids(scan_cols))) {
    } else {
      for (int64_t i = 0; OB_SUCC(ret) && i < text_col_ids_.count(); ++i) {
        const share::schema::ObColumnSchemaV2 *col = data_schema_->get_column_schema(text_col_ids_.at(i));
        int64_t pos = -1;
        if (OB_ISNULL(col)) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("fts indexed column not found", K(ret), K(text_col_ids_.at(i)));
        } else if (CS_TYPE_INVALID == text_collation_) {
          text_collation_ = col->get_collation_type();
        }
        for (int64_t j = 0; OB_SUCC(ret) && -1 == pos && j < scan_cols.count(); ++j) {
          if (scan_cols.at(j).col_id_ == text_col_ids_.at(i)) {
            pos = j;
          }
        }
        if (OB_SUCC(ret) && -1 == pos) {
          ret = OB_ERR_UNEXPECTED;
          LOG_WARN("fts indexed column not in scan output", K(ret), K(text_col_ids_.at(i)));
        } else if (OB_FAIL(text_col_pos_.push_back(pos))) {
        }
      }
    }
    if (OB_SUCC(ret)) {
      // The document id must be the exact value the stock build stores, so read
      // it from the data table's hidden doc-id column.
      if (OB_SUCCESS != data_schema_->get_docid_col_id(doc_id_col_id_)
          || OB_INVALID_ID == doc_id_col_id_) {
        doc_id_col_id_ = OB_INVALID_ID;
        ret = OB_NOT_SUPPORTED;
        LOG_INFO("fts direct build needs a table-sequence document id", KPC(data_schema_));
      }
    }
    LOG_INFO("fts aux build columns resolved", K(ret), K(word_col_id), K(word_collation_),
             K(text_col_ids_), K(text_col_pos_), K(doc_id_col_id_),
             K(aux_schema_->get_parser_name_str()));
  }
  return ret;
}

int ObFtsAuxProducer::tokenize_text(const common::ObString &text,
                                    storage::ObFTWordMap &word_map,
                                    int64_t &doc_length)
{
  int ret = OB_SUCCESS;
  common::ObObjMeta meta;
  meta.set_varchar();
  meta.set_collation_type(text_collation_);
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
  } else if (0 == text.length()) {
    doc_length = 0;
  } else if (OB_FAIL(helper_.segment(meta, text.ptr(), text.length(), doc_length, word_map))) {
  }
  return ret;
}


int ObFtsAuxProducer::append_select_columns(common::ObSqlString &sql) const
{
  int ret = OB_SUCCESS;
  for (int64_t i = 0; OB_SUCC(ret) && i < text_col_ids_.count(); ++i) {
    const share::schema::ObColumnSchemaV2 *col = data_schema_->get_column_schema(text_col_ids_.at(i));
    if (OB_ISNULL(col)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(sql.append_fmt("%s`%.*s`", 0 == i ? "" : ", ",
                                      col->get_column_name_str().length(),
                                      col->get_column_name_str().ptr()))) {
    }
  }
  const common::ObRowkeyInfo &rowkey_info = data_schema_->get_rowkey_info();
  for (int64_t i = 0; OB_SUCC(ret) && i < rowkey_info.get_size(); ++i) {
    uint64_t column_id = OB_INVALID_ID;
    const share::schema::ObColumnSchemaV2 *col = nullptr;
    if (OB_FAIL(rowkey_info.get_column_id(i, column_id))) {
    } else if (OB_ISNULL(col = data_schema_->get_column_schema(column_id))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(sql.append_fmt("%s`%.*s`", 0 == text_col_ids_.count() && 0 == i ? "" : ", ",
                                      col->get_column_name_str().length(),
                                      col->get_column_name_str().ptr()))) {
    }
  }
  if (OB_SUCC(ret)) {
    const share::schema::ObColumnSchemaV2 *doc_col =
        data_schema_->get_column_schema(doc_id_col_id_);
    if (OB_ISNULL(doc_col)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(sql.append_fmt(", `%.*s`",
                                      doc_col->get_column_name_str().length(),
                                      doc_col->get_column_name_str().ptr()))) {
    }
  }
  return ret;
}

namespace
{
struct ObFtsAuxSliceCtx;
typedef int (*ObFtsAuxSliceFn)(ObFtsAuxSliceCtx &ctx);

struct ObFtsAuxSliceCtx
{
  ObFtsAuxSliceCtx()
    : fn_(nullptr), lo_(0), hi_(0), offset_(0), alloc_(nullptr),
      doc_cnt_(0), token_cnt_(0), ret_(OB_SUCCESS), scan_us_(0), sort_us_(0)
  {}
  ObFtsAuxSliceFn fn_;
  int64_t lo_;
  int64_t hi_;
  int64_t offset_;
  common::ObArenaAllocator *alloc_;
  common::ObSEArray<ObFtsAuxRowRec, 1024> rows_;
  int64_t doc_cnt_;
  int64_t token_cnt_;
  int ret_;
  int64_t scan_us_;
  int64_t sort_us_;
};

struct ObFtsAuxRowLess
{
  explicit ObFtsAuxRowLess(const ObCollationType cs) : cs_(cs) {}
  bool operator()(const ObFtsAuxRowRec &l, const ObFtsAuxRowRec &r) const
  {
    const int cmp = ObCharset::strcmpsp(cs_, l.word_, l.word_len_, r.word_, r.word_len_, true);
    if (0 != cmp) {
      return cmp < 0;
    }
    return memcmp(l.doc_id_str_, r.doc_id_str_, OB_DOC_ID_COLUMN_BYTE_LENGTH) < 0;
  }
  ObCollationType cs_;
};
} // namespace

int ObFtsAuxProducer::read_key_bounds(int64_t &min_key, int64_t &max_key)
{
  int ret = OB_SUCCESS;
  uint64_t key_col_id = OB_INVALID_ID;
  const share::schema::ObColumnSchemaV2 *key_col = nullptr;
  const common::ObRowkeyInfo &rowkey_info = data_schema_->get_rowkey_info();
  common::ObSqlString sql;
  min_key = 0;
  max_key = 0;
  if (OB_UNLIKELY(1 != rowkey_info.get_size())) {
    ret = OB_NOT_SUPPORTED;
  } else if (OB_FAIL(rowkey_info.get_column_id(0, key_col_id))) {
  } else if (OB_ISNULL(key_col = data_schema_->get_column_schema(key_col_id))) {
    ret = OB_ERR_UNEXPECTED;
  } else if (!key_col->get_meta_type().is_integer_type()) {
    ret = OB_NOT_SUPPORTED;
  } else if (OB_FAIL(sql.append_fmt("SELECT MIN(`%.*s`), MAX(`%.*s`) FROM %s AS OF SNAPSHOT %ld",
                                    key_col->get_column_name_str().length(),
                                    key_col->get_column_name_str().ptr(),
                                    key_col->get_column_name_str().length(),
                                    key_col->get_column_name_str().ptr(),
                                    data_table_name_.ptr(), snapshot_version_))) {
  } else {
    common::ObMySQLProxy::MySQLResult res;
    common::sqlclient::ObMySQLResult *result = nullptr;
    if (OB_FAIL(GCTX.ddl_sql_proxy_->read(res, sql.ptr()))) {
      LOG_WARN("fts aux read key bounds failed", K(ret), K(sql));
    } else if (OB_ISNULL(result = res.get_result())) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(result->next())) {
    } else if (OB_FAIL(result->get_int(static_cast<int64_t>(0), min_key)) || OB_FAIL(result->get_int(static_cast<int64_t>(1), max_key))) {
    }
  }
  if (OB_NOT_SUPPORTED == ret) {
    ret = OB_SUCCESS;
    min_key = 1;
    max_key = 0; // signals "do not slice"
  }
  return ret;
}

int ObFtsAuxProducer::build_tablet_parallel(const int64_t slice_cnt,
                                            common::ObIArray<ObFtsAuxRowRec> &rows,
                                            int64_t &doc_cnt,
                                            int64_t &token_cnt)
{
  int ret = OB_SUCCESS;
  int64_t min_key = 0;
  int64_t max_key = 0;
  doc_cnt = 0;
  token_cnt = 0;
  if (OB_UNLIKELY(!is_inited_) || slice_cnt <= 0) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(read_key_bounds(min_key, max_key))) {
  } else if (max_key < min_key || (max_key - min_key + 1) < slice_cnt * 32) {
    ret = OB_NOT_SUPPORTED;
  } else {
    const int64_t span = (max_key - min_key + 1 + slice_cnt - 1) / slice_cnt;
    uint64_t key_col_id = OB_INVALID_ID;
    const share::schema::ObColumnSchemaV2 *key_col = nullptr;
    std::vector<ObFtsAuxSliceCtx> slices(static_cast<size_t>(slice_cnt));
    int64_t offset = 0;
    if (OB_FAIL(data_schema_->get_rowkey_info().get_column_id(0, key_col_id))
        || OB_ISNULL(key_col = data_schema_->get_column_schema(key_col_id))) {
      ret = OB_ERR_UNEXPECTED;
    }
    for (int64_t i = 0; OB_SUCC(ret) && i < slice_cnt; ++i) {
      ObFtsAuxSliceCtx &ctx = slices[static_cast<size_t>(i)];
      ctx.lo_ = min_key + i * span;
      ctx.hi_ = (i + 1 == slice_cnt) ? (max_key + 1) : (min_key + (i + 1) * span);
      if (i > 0) {
        common::ObSqlString cnt_sql;
        common::ObMySQLProxy::MySQLResult res;
        common::sqlclient::ObMySQLResult *result = nullptr;
        if (OB_FAIL(cnt_sql.append_fmt("SELECT COUNT(*) FROM %s AS OF SNAPSHOT %ld WHERE `%.*s` < %ld",
                                       data_table_name_.ptr(), snapshot_version_,
                                       key_col->get_column_name_str().length(),
                                       key_col->get_column_name_str().ptr(), ctx.lo_))) {
        } else if (OB_FAIL(GCTX.ddl_sql_proxy_->read(res, cnt_sql.ptr()))) {
        } else if (OB_ISNULL(result = res.get_result())) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_FAIL(result->next())) {
        } else if (OB_FAIL(result->get_int(static_cast<int64_t>(0), offset))) {
        }
      }
      ctx.offset_ = offset;
    }
    if (OB_SUCC(ret)) {
      for (int64_t i = 0; i < slice_cnt; ++i) {
        ObFtsAuxSliceCtx &ctx = slices[static_cast<size_t>(i)];
        if (OB_ISNULL(ctx.alloc_ = OB_NEW(ObArenaAllocator, ObMemAttr("FtsAuxSlice"),
                                          ObMemAttr("FtsAuxSlice")))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
          break;
        }
      }
    }
    if (OB_SUCC(ret)) {
      // Each slice: read + tokenize + local sort on its own thread.
      std::vector<std::thread> workers;
      workers.reserve(static_cast<size_t>(slice_cnt));
      for (int64_t i = 0; i < slice_cnt; ++i) {
        ObFtsAuxSliceCtx *ctx = &slices[static_cast<size_t>(i)];
        workers.emplace_back([this, ctx, key_col]() {
          common::ObSqlString sql;
          storage::ObFTWordMap word_map;
          common::ObObjMeta meta;
          meta.set_varchar();
          meta.set_collation_type(text_collation_);
          storage::ObFTParseHelper helper;
          int &ret = ctx->ret_;
          const int64_t t0 = common::ObTimeUtility::current_time();
          if (OB_FAIL(sql.append("SELECT ")) || OB_FAIL(append_select_columns(sql))
              || OB_FAIL(sql.append_fmt(" FROM %s AS OF SNAPSHOT %ld WHERE `%.*s` >= %ld AND `%.*s` < %ld ORDER BY %ld",
                                        data_table_name_.ptr(), snapshot_version_,
                                        key_col->get_column_name_str().length(),
                                        key_col->get_column_name_str().ptr(), ctx->lo_,
                                        key_col->get_column_name_str().length(),
                                        key_col->get_column_name_str().ptr(), ctx->hi_,
                                        text_col_ids_.count() + 1))) {
          } else if (OB_FAIL(helper.init(ctx->alloc_, get_parser_name(), get_parser_props()))) {
          } else if (OB_FAIL(word_map.create(256, common::ObMemAttr("FtsAuxWordMap")))) {
          } else {
            common::ObMySQLProxy::MySQLResult res;
            common::sqlclient::ObMySQLResult *result = nullptr;
            if (OB_FAIL(GCTX.ddl_sql_proxy_->read(res, sql.ptr()))) {
              LOG_WARN("fts aux slice read failed", K(ret), K(sql));
            } else if (OB_ISNULL(result = res.get_result())) {
              ret = OB_ERR_UNEXPECTED;
            } else {
              int64_t local_idx = 0;
              while (OB_SUCC(result->next())) {
                int64_t total_len = -1;
                common::ObSEArray<common::ObString, 4> parts;
                ObString part;
                for (int64_t c = 0; OB_SUCC(ret) && c < text_col_ids_.count(); ++c) {
                  if (OB_FAIL(result->get_varchar(c, part))) {
                  } else if (OB_FAIL(parts.push_back(part))) {
                  } else {
                    total_len += part.length() + 1;
                  }
                }
                ObString doc_id_str;
                char *doc_id_buf = nullptr;
                const int64_t doc_id_pos =
                    text_col_ids_.count() + data_schema_->get_rowkey_info().get_size();
                if (OB_FAIL(ret)) {
                } else if (OB_FAIL(result->get_varchar(doc_id_pos, doc_id_str))) {
                  LOG_WARN("fts aux slice read doc id failed", K(ret));
                } else if (OB_UNLIKELY(doc_id_str.length() < OB_DOC_ID_COLUMN_BYTE_LENGTH)) {
                  ret = OB_ERR_UNEXPECTED;
                } else if (OB_ISNULL(doc_id_buf = static_cast<char *>(ctx->alloc_->alloc(OB_DOC_ID_COLUMN_BYTE_LENGTH)))) {
                  ret = OB_ALLOCATE_MEMORY_FAILED;
                } else {
                  MEMCPY(doc_id_buf, doc_id_str.ptr(), OB_DOC_ID_COLUMN_BYTE_LENGTH);
                }
                char *text_buf = nullptr;
                int64_t pos = 0;
                if (OB_FAIL(ret)) {
                } else if (OB_ISNULL(text_buf = static_cast<char *>(ctx->alloc_->alloc(total_len + 1)))) {
                  ret = OB_ALLOCATE_MEMORY_FAILED;
                } else {
                  for (int64_t p = 0; OB_SUCC(ret) && p < parts.count(); ++p) {
                    if (0 != p) {
                      text_buf[pos++] = ' ';
                    }
                    if (!parts.at(p).empty()) {
                      MEMCPY(text_buf + pos, parts.at(p).ptr(), parts.at(p).length());
                      pos += parts.at(p).length();
                    }
                  }
                  int64_t doc_length = 0;
                  if (0 == pos) {
                    doc_length = 0;
                  } else if (OB_FAIL(helper.segment(meta, text_buf, pos, doc_length, word_map))) {
                  }
                  if (OB_SUCC(ret)) {
                    for (storage::ObFTWordMap::const_iterator iter = word_map.begin();
                         OB_SUCC(ret) && iter != word_map.end(); ++iter) {
                      const common::ObString word = iter->first.get_word().get_string();
                      char *word_buf = nullptr;
                      if (word.empty()) {
                      } else if (OB_ISNULL(word_buf = static_cast<char *>(ctx->alloc_->alloc(word.length())))) {
                        ret = OB_ALLOCATE_MEMORY_FAILED;
                      } else {
                        MEMCPY(word_buf, word.ptr(), word.length());
                        ObFtsAuxRowRec rec;
                        rec.word_ = word_buf;
                        rec.word_len_ = word.length();
                        rec.doc_id_str_ = doc_id_buf;
                        rec.doc_id_ = ctx->offset_ + local_idx;
                        rec.word_count_ = iter->second;
                        rec.doc_length_ = doc_length;
                        if (OB_FAIL(ctx->rows_.push_back(rec))) {
                        }
                      }
                    }
                    ctx->token_cnt_ += word_map.size();
                    word_map.reuse();
                  }
                }
                if (OB_SUCC(ret)) {
                  ++local_idx;
                }
              }
              if (OB_ITER_END == ret) {
                ret = OB_SUCCESS;
              }
              ctx->doc_cnt_ = local_idx;
            }
          }
          ctx->scan_us_ = common::ObTimeUtility::current_time() - t0;
          if (OB_SUCC(ret)) {
            const int64_t t1 = common::ObTimeUtility::current_time();
            ObFtsAuxRowLess less(word_collation_);
            std::sort(ctx->rows_.begin(), ctx->rows_.end(), less);
            ctx->sort_us_ = common::ObTimeUtility::current_time() - t1;
          }
          if (word_map.created()) {
            word_map.destroy();
          }
        });
      }
      for (size_t i = 0; i < workers.size(); ++i) {
        if (workers[i].joinable()) {
          workers[i].join();
        }
      }
      for (int64_t i = 0; OB_SUCC(ret) && i < slice_cnt; ++i) {
        ObFtsAuxSliceCtx &ctx = slices[static_cast<size_t>(i)];
        LOG_INFO("fts aux slice done", "slice", i, K(ctx.ret_), K(ctx.doc_cnt_), K(ctx.token_cnt_),
                 K(ctx.rows_.count()), "scan_us", ctx.scan_us_, "sort_us", ctx.sort_us_);
        if (OB_SUCCESS != ctx.ret_) {
          ret = ctx.ret_;
        }
      }
    }
    // k-way merge of the per-slice sorted runs.
    const int64_t merge_t0 = common::ObTimeUtility::current_time();
    if (OB_SUCC(ret)) {
      // The heap holds (slice, row) indices only: a merge step costs one record
      // copy into the output and ~log2(k) collation compares, with no
      // std::function indirection.
      ObFtsAuxRowLess less(word_collation_);
      typedef std::pair<int64_t, int64_t> HeapSlot;  // <slice, row index>
      std::vector<HeapSlot> heap;
      heap.reserve(static_cast<size_t>(slice_cnt));
      const auto slot_row = [&slices](const HeapSlot &slot) -> const ObFtsAuxRowRec & {
        return slices[static_cast<size_t>(slot.first)].rows_.at(slot.second);
      };
      const auto heap_less = [&less, &slot_row](const HeapSlot &a, const HeapSlot &b) {
        return less(slot_row(a), slot_row(b));
      };
      for (int64_t i = 0; i < slice_cnt; ++i) {
        if (slices[static_cast<size_t>(i)].rows_.count() > 0) {
          heap.push_back(HeapSlot(i, 0));
        }
      }
      const auto sift_down = [&heap, &heap_less](int64_t pos) {
        const int64_t n = static_cast<int64_t>(heap.size());
        while (true) {
          int64_t smallest = pos;
          const int64_t l = 2 * pos + 1;
          const int64_t r = l + 1;
          if (l < n && heap_less(heap[static_cast<size_t>(l)], heap[static_cast<size_t>(smallest)])) {
            smallest = l;
          }
          if (r < n && heap_less(heap[static_cast<size_t>(r)], heap[static_cast<size_t>(smallest)])) {
            smallest = r;
          }
          if (smallest == pos) {
            break;
          }
          std::swap(heap[static_cast<size_t>(pos)], heap[static_cast<size_t>(smallest)]);
          pos = smallest;
        }
      };
      for (int64_t i = static_cast<int64_t>(heap.size()) / 2 - 1; i >= 0; --i) {
        sift_down(i);
      }
      while (OB_SUCC(ret) && !heap.empty()) {
        const HeapSlot top = heap[0];
        if (OB_FAIL(rows.push_back(slot_row(top)))) {
          break;
        }
        const int64_t slice = top.first;
        if (top.second + 1 < slices[static_cast<size_t>(slice)].rows_.count()) {
          heap[0].second = top.second + 1;
          sift_down(0);
        } else {
          heap[0] = heap.back();
          heap.pop_back();
          if (!heap.empty()) {
            sift_down(0);
          }
        }
      }
      for (int64_t i = 0; i < slice_cnt; ++i) {
        doc_cnt += slices[static_cast<size_t>(i)].doc_cnt_;
        token_cnt += slices[static_cast<size_t>(i)].token_cnt_;
      }
      for (int64_t i = 0; i < slice_cnt; ++i) {
        slice_allocs_.push_back(slices[static_cast<size_t>(i)].alloc_);
      }
    } else {
      for (int64_t i = 0; i < slice_cnt; ++i) {
        if (nullptr != slices[static_cast<size_t>(i)].alloc_) {
          OB_DELETE(ObArenaAllocator, ObMemAttr("FtsAuxSlice"), slices[static_cast<size_t>(i)].alloc_);
        }
      }
    }
    LOG_INFO("fts aux parallel build", K(ret), K(slice_cnt), K(doc_cnt), K(token_cnt),
             K(rows.count()), "merge_us", common::ObTimeUtility::current_time() - merge_t0);
  }
  return ret;
}
int ObFtsAuxProducer::build_tablet(const common::ObTabletID &tablet_id,
                                   common::ObIArray<ObFtsAuxRowRec> &rows,
                                   int64_t &doc_cnt,
                                   int64_t &token_cnt)
{
  int ret = OB_SUCCESS;
  doc_cnt = 0;
  token_cnt = 0;
  common::ObSqlString sql;
  storage::ObFTWordMap word_map;
  ObArenaAllocator text_alloc(lib::ObMemAttr("FtsAuxText"));
  if (OB_UNLIKELY(!is_inited_)) {
    ret = OB_NOT_INIT;
  } else if (OB_FAIL(word_map.create(256, common::ObMemAttr("FtsAuxWordMap")))) {
  } else if (OB_FAIL(sql.append("SELECT "))) {
  }
  // Indexed columns, then the primary key columns (the scan order defines the
  // document id, so it must be explicit).
  for (int64_t i = 0; OB_SUCC(ret) && i < text_col_ids_.count(); ++i) {
    const share::schema::ObColumnSchemaV2 *col = data_schema_->get_column_schema(text_col_ids_.at(i));
    if (OB_ISNULL(col)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(sql.append_fmt("%s`%.*s`", 0 == i ? "" : ", ",
                                      col->get_column_name_str().length(),
                                      col->get_column_name_str().ptr()))) {
    }
  }
  const common::ObRowkeyInfo &rowkey_info = data_schema_->get_rowkey_info();
  for (int64_t i = 0; OB_SUCC(ret) && i < rowkey_info.get_size(); ++i) {
    uint64_t column_id = OB_INVALID_ID;
    const share::schema::ObColumnSchemaV2 *col = nullptr;
    if (OB_FAIL(rowkey_info.get_column_id(i, column_id))) {
    } else if (OB_ISNULL(col = data_schema_->get_column_schema(column_id))) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(sql.append_fmt("%s`%.*s`", 0 == text_col_ids_.count() && 0 == i ? "" : ", ",
                                      col->get_column_name_str().length(),
                                      col->get_column_name_str().ptr()))) {
    }
  }
  if (OB_FAIL(ret)) {
  } else if (OB_FAIL(sql.append_fmt(" FROM %s AS OF SNAPSHOT %ld ORDER BY %ld",
                                    data_table_name_.ptr(), snapshot_version_,
                                    text_col_ids_.count() + 1))) {
  } else {
    // ORDER BY the first primary key column keeps the document order stable;
    // additional key columns only matter for composite keys, which the caller
    // rejects.
    common::ObMySQLProxy::MySQLResult res;
    common::sqlclient::ObMySQLResult *result = nullptr;
    if (OB_ISNULL(GCTX.ddl_sql_proxy_)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(GCTX.ddl_sql_proxy_->read(res, sql.ptr()))) {
      LOG_WARN("fts aux read source failed", K(ret), K(sql));
    } else if (OB_ISNULL(result = res.get_result())) {
      ret = OB_ERR_UNEXPECTED;
    } else {
      int64_t row_idx = 0;
      while (OB_SUCC(result->next())) {
        int64_t total_len = -1;
        common::ObSEArray<common::ObString, 4> parts;
        ObString part;
        for (int64_t i = 0; OB_SUCC(ret) && i < text_col_ids_.count(); ++i) {
          if (OB_FAIL(result->get_varchar(i, part))) {
            LOG_WARN("fts aux read column failed", K(ret), K(i));
          } else if (OB_FAIL(parts.push_back(part))) {
          } else {
            total_len += part.length() + 1;
          }
        }
        ObString doc_id_str;
        char *doc_id_buf = nullptr;
        const int64_t doc_id_pos =
            text_col_ids_.count() + data_schema_->get_rowkey_info().get_size();
        if (OB_FAIL(ret)) {
        } else if (OB_FAIL(result->get_varchar(doc_id_pos, doc_id_str))) {
          LOG_WARN("fts aux read doc id failed", K(ret));
        } else if (OB_UNLIKELY(doc_id_str.length() < OB_DOC_ID_COLUMN_BYTE_LENGTH)) {
          ret = OB_ERR_UNEXPECTED;
        } else if (OB_ISNULL(doc_id_buf = static_cast<char *>(alloc_.alloc(OB_DOC_ID_COLUMN_BYTE_LENGTH)))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          MEMCPY(doc_id_buf, doc_id_str.ptr(), OB_DOC_ID_COLUMN_BYTE_LENGTH);
        }
        char *text_buf = nullptr;
        int64_t pos = 0;
        if (OB_FAIL(ret)) {
        } else if (OB_ISNULL(text_buf = static_cast<char *>(text_alloc.alloc(total_len + 1)))) {
          ret = OB_ALLOCATE_MEMORY_FAILED;
        } else {
          for (int64_t i = 0; OB_SUCC(ret) && i < parts.count(); ++i) {
            if (0 != i) {
              text_buf[pos++] = ' ';
            }
            if (!parts.at(i).empty()) {
              MEMCPY(text_buf + pos, parts.at(i).ptr(), parts.at(i).length());
              pos += parts.at(i).length();
            }
          }
          int64_t doc_length = 0;
          if (OB_FAIL(tokenize_text(common::ObString(static_cast<int32_t>(pos), text_buf),
                                    word_map, doc_length))) {
          } else {
            for (storage::ObFTWordMap::const_iterator iter = word_map.begin();
                 OB_SUCC(ret) && iter != word_map.end(); ++iter) {
              const common::ObString word = iter->first.get_word().get_string();
              char *word_buf = nullptr;
              if (word.empty()) {
              } else if (OB_ISNULL(word_buf = static_cast<char *>(alloc_.alloc(word.length())))) {
                ret = OB_ALLOCATE_MEMORY_FAILED;
              } else {
                MEMCPY(word_buf, word.ptr(), word.length());
                ObFtsAuxRowRec rec;
                rec.word_ = word_buf;
                rec.word_len_ = word.length();
                rec.doc_id_str_ = doc_id_buf;
                rec.doc_id_ = row_idx;
                rec.word_count_ = iter->second;
                rec.doc_length_ = doc_length;
                if (OB_FAIL(rows.push_back(rec))) {
                }
              }
            }
          }
          if (OB_SUCC(ret)) {
            ++row_idx;
            ++doc_cnt;
            token_cnt += word_map.size();
          }
        }
      }
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
      }
      LOG_INFO("fts aux read source done", K(ret), K(row_idx), K(rows.count()));
    }
  }
  if (OB_FAIL(ret)) {
    LOG_WARN("fts aux producer failed", K(ret), K(tablet_id), K(doc_cnt), K(token_cnt));
  }
  if (word_map.created()) {
    word_map.destroy();
  }
  return ret;
}

} // anonymous namespace

namespace oceanbase
{
namespace storage
{


namespace
{
// Rows are pushed straight into the DAG writer in batches; the write macro block
// operator appends them to the aux tablet's sstables.
int write_fts_aux_rows(storage::ObDDLIndependentDag *dag,
                       const common::ObTabletID &tablet_id,
                       const common::ObIArray<ObFtsAuxRowRec> &rows,
                       common::ObIAllocator &alloc)
{
  int ret = OB_SUCCESS;
  int64_t slice_cnt = 6;
  const char *wslice_env = getenv("FTS_AUX_WSLICES");
  if (nullptr != wslice_env && '\0' != wslice_env[0]) {
    slice_cnt = atoll(wslice_env);
  }
  if (slice_cnt < 1) {
    slice_cnt = 1;
  }
  UNUSED(alloc);
  if (OB_ISNULL(dag) || rows.empty()) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    ObDDLTabletContext *tablet_context = nullptr;
    if (OB_FAIL(dag->get_tablet_context(tablet_id, tablet_context))) {
    } else if (OB_ISNULL(tablet_context)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (FALSE_IT(tablet_context->slice_count_ = slice_cnt)) {
    } else {
      struct SliceArg
      {
        storage::ObDDLIndependentDag *dag_;
        common::ObTabletID tablet_id_;
        const common::ObIArray<ObFtsAuxRowRec> *rows_;
        int64_t begin_;
        int64_t end_;
        int64_t slice_idx_;
        int64_t slice_cnt_;
        int64_t written_;
        int ret_;
      };
      std::vector<SliceArg> args(static_cast<size_t>(slice_cnt));
      std::vector<std::thread> workers;
      workers.reserve(static_cast<size_t>(slice_cnt));
      const int64_t per_slice = (rows.count() + slice_cnt - 1) / slice_cnt;
      for (int64_t s = 0; s < slice_cnt; ++s) {
        SliceArg &arg = args[static_cast<size_t>(s)];
        arg.dag_ = dag;
        arg.tablet_id_ = tablet_id;
        arg.rows_ = &rows;
        arg.begin_ = MIN(s * per_slice, rows.count());
        arg.end_ = MIN((s + 1) * per_slice, rows.count());
        arg.slice_idx_ = s;
        arg.slice_cnt_ = slice_cnt;
        arg.written_ = 0;
        arg.ret_ = OB_SUCCESS;
        const int64_t t0 = common::ObTimeUtility::current_time();
        workers.emplace_back([&arg, t0]() {
          storage::ObWriteMacroParam param;
          storage::ObRsSliceWriter *writer = nullptr;
          common::ObArenaAllocator thread_alloc(lib::ObMemAttr("FtsAuxSliceW"));
          blocksstable::ObStorageDatum datums[4];
          common::ObSEArray<ObDatum *, 4> datum_ptrs;
          int &ret = arg.ret_;
          if (arg.begin_ >= arg.end_) {
            // nothing to write for this slice
          } else if (OB_FAIL(storage::ObDDLStorageUtil::fill_writer_param(
                       arg.tablet_id_, arg.slice_idx_, arg.dag_, 0 /* max_batch_size */, param))) {
            LOG_WARN("failed to fill aux slice writer param", K(ret), K(arg.slice_idx_));
          } else if (OB_ISNULL(writer = OB_NEWx(storage::ObRsSliceWriter, &thread_alloc))) {
            ret = OB_ALLOCATE_MEMORY_FAILED;
          } else {
            param.slice_count_ = arg.slice_cnt_;
            if (OB_FAIL(writer->init(param))) {
              LOG_WARN("failed to init aux slice writer", K(ret), K(arg.slice_idx_));
            } else {
              for (int64_t i = arg.begin_; OB_SUCC(ret) && i < arg.end_; ++i) {
                const ObFtsAuxRowRec &row = arg.rows_->at(i);
                datums[0].set_string(row.word_, row.word_len_);
                datums[1].set_string(row.doc_id_str_, OB_DOC_ID_COLUMN_BYTE_LENGTH);
                datums[2].set_int(row.word_count_);
                datums[3].set_int(row.doc_length_);
                datum_ptrs.reuse();
                for (int64_t c = 0; OB_SUCC(ret) && c < 4; ++c) {
                  if (OB_FAIL(datum_ptrs.push_back(&datums[c]))) {
                  }
                }
                if (OB_SUCC(ret) && OB_FAIL(writer->append_current_row(datum_ptrs))) {
                  LOG_WARN("failed to append aux row", K(ret), K(i), K(arg.slice_idx_));
                }
              }
              if (OB_SUCC(ret)) {
                arg.written_ = arg.end_ - arg.begin_;
                if (OB_FAIL(writer->close())) {
                  LOG_WARN("failed to close aux slice writer", K(ret), K(arg.slice_idx_));
                }
              }
            }
          }
          LOG_INFO("fts aux direct step", "step", "slice_writer_done", K(arg.slice_idx_),
                   "written", arg.written_, "ret", ret,
                   "cost_us", common::ObTimeUtility::current_time() - t0);
        });
      }
      for (size_t i = 0; i < workers.size(); ++i) {
        if (workers[i].joinable()) {
          workers[i].join();
        }
      }
      for (int64_t s = 0; s < slice_cnt; ++s) {
        if (OB_SUCCESS != args[static_cast<size_t>(s)].ret_) {
          ret = args[static_cast<size_t>(s)].ret_;
        }
      }
      LOG_INFO("fts aux direct step", "step", "writers_joined", K(ret), K(slice_cnt),
               "rows", rows.count());
    }
  }
  return ret;
}

} // namespace

int build_fts_inverted_aux_by_scan(const ObFtsAuxBuildParam &param)
{
  int ret = OB_SUCCESS;
  share::schema::ObSchemaGetterGuard schema_guard;
  const share::schema::ObTableSchema *aux_schema = nullptr;
  const share::schema::ObTableSchema *data_schema = nullptr;
  common::ObSEArray<common::ObTabletID, 4> tablet_ids;
  char qualified_name[2 * OB_MAX_USER_TABLE_NAME_LENGTH_MYSQL + 8] = {0};
  int32_t qualified_len = 0;
  int64_t start_time = 0;
  if (!fts_aux_direct_enabled() && !fts_aux_dryrun_enabled()) {
    // Stock inner-SQL build: do not touch the schema or the tablets at all.
    return OB_NOT_SUPPORTED;
  }
  if (OB_UNLIKELY(!param.is_valid())) {
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(GCTX.schema_service_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(GCTX.schema_service_->get_runtime_schema_guard(schema_guard))) {
  } else if (OB_FAIL(schema_guard.get_table_schema(param.aux_table_id_, aux_schema))) {
  } else if (OB_ISNULL(aux_schema)) {
    ret = OB_TABLE_NOT_EXIST;
    LOG_WARN("fts aux table not found", K(ret), K(param));
  } else if (OB_FAIL(schema_guard.get_table_schema(param.data_table_id_, data_schema))) {
  } else if (OB_ISNULL(data_schema)) {
    ret = OB_TABLE_NOT_EXIST;
    LOG_WARN("fts data table not found", K(ret), K(param));
  } else if (OB_FAIL(ObDDLUtil::get_tablets(*GCTX.schema_service_, param.aux_table_id_, tablet_ids))) {
  } else if (OB_UNLIKELY(tablet_ids.count() != 1)) {
    // The document id is the scan-order row index, which only lines up with the
    // other aux children for an unpartitioned source table.
    ret = OB_NOT_SUPPORTED;
    LOG_INFO("fts aux direct build skipped for partitioned source", K(param), K(tablet_ids.count()));
  } else {
    const share::schema::ObDatabaseSchema *db_schema = nullptr;
    common::ObString db_name;
    int64_t qualified_pos = 0;
    if (OB_FAIL(schema_guard.get_database_schema(data_schema->get_database_id(), db_schema))) {
    } else if (OB_ISNULL(db_schema)) {
      ret = OB_ERR_UNEXPECTED;
    } else if (FALSE_IT(db_name = db_schema->get_database_name_str())) {
    } else if (OB_FAIL(databuff_printf(qualified_name, sizeof(qualified_name), qualified_pos,
                                       "`%.*s`.`%.*s`",
                                       db_name.length(), db_name.ptr(),
                                       data_schema->get_table_name_str().length(),
                                       data_schema->get_table_name_str().ptr()))) {
    } else {
      qualified_len = static_cast<int32_t>(qualified_pos);
    }
  }
  if (OB_SUCC(ret)) {
    start_time = common::ObTimeUtility::current_time();
    LOG_INFO("fts aux scan build begin", K(param), K(tablet_ids.count()),
             "direct_build_enabled", fts_aux_direct_enabled(), "dryrun", fts_aux_dryrun_enabled());
  }
  if (OB_SUCC(ret) && fts_aux_dryrun_enabled()) {
    ObFtsAuxProducer producer;
    LOG_INFO("fts aux producer init start");
    producer.set_data_table_name(common::ObString(qualified_len, qualified_name));
    const int init_ret = producer.init(*data_schema, *aux_schema, param.snapshot_version_);
    if (OB_SUCCESS != init_ret) {
      // A dry run only measures the producer; it must never fail the DDL.
      LOG_WARN("fts aux producer init failed (dry run)", K(init_ret));
      ret = OB_SUCCESS;
    } else {
      LOG_INFO("fts aux producer init done");
      for (int64_t i = 0; OB_SUCC(ret) && i < tablet_ids.count(); ++i) {
        common::ObSEArray<ObFtsAuxRowRec, 1024> rows;
        int64_t doc_cnt = 0;
        int64_t token_cnt = 0;
        int64_t t0 = common::ObTimeUtility::current_time();
        int64_t slice_cnt = 6;
        const char *slice_env = getenv("FTS_AUX_SLICES");
        if (nullptr != slice_env && '\0' != slice_env[0]) {
          slice_cnt = atoll(slice_env);
        }
        int build_ret = producer.build_tablet_parallel(slice_cnt, rows, doc_cnt, token_cnt);
        if (OB_NOT_SUPPORTED == build_ret) {
          build_ret = producer.build_tablet(tablet_ids.at(i), rows, doc_cnt, token_cnt);
        }
        if (OB_SUCCESS != build_ret) {
          LOG_WARN("fts aux producer build tablet failed (dry run)", K(build_ret),
                   "tablet_id", tablet_ids.at(i));
          ret = OB_SUCCESS;
        } else {
          int64_t t1 = common::ObTimeUtility::current_time();
          const ObCollationType cs = producer.get_word_collation();
          std::sort(rows.begin(), rows.end(), [cs](const ObFtsAuxRowRec &l, const ObFtsAuxRowRec &r) {
            const int cmp = ObCharset::strcmpsp(cs, l.word_, l.word_len_, r.word_, r.word_len_, true);
            return cmp < 0 ? true : (cmp > 0 ? false : l.doc_id_ < r.doc_id_);
          });
          int64_t t2 = common::ObTimeUtility::current_time();
          LOG_INFO("fts aux scan build tablet", K(ret), "tablet_id", tablet_ids.at(i),
                   K(doc_cnt), K(token_cnt), K(rows.count()),
                   "scan_tokenize_us", t1 - t0, "sort_us", t2 - t1);
        }
      }
    }
  }
  bool direct_wrote = false;
  if (OB_SUCC(ret) && fts_aux_direct_enabled()) {
    // Build the inverted rows directly and write them through the DDL insert DAG.
    int64_t slice_cnt = 6;
    const char *slice_env = getenv("FTS_AUX_SLICES");
    if (nullptr != slice_env && '\0' != slice_env[0]) {
      slice_cnt = atoll(slice_env);
    }
    ObFtsAuxProducer producer;
    common::ObSEArray<ObFtsAuxRowRec, 1024> rows;
    int64_t doc_cnt = 0;
    int64_t token_cnt = 0;
    producer.set_data_table_name(common::ObString(qualified_len, qualified_name));
    if (OB_FAIL(producer.init(*data_schema, *aux_schema, param.snapshot_version_))) {
      LOG_WARN("fts aux direct producer init failed", K(ret));
    } else if (OB_FAIL(producer.build_tablet_parallel(slice_cnt, rows, doc_cnt, token_cnt))) {
    } else {
      share::ObDDLTaskDataInfo task_data_info;
      storage::ObDDLInsertDagInitParam dag_param;
      ObArenaAllocator dag_alloc(lib::ObMemAttr("FtsAuxDag"));
      ObArenaAllocator batch_alloc(lib::ObMemAttr("FtsAuxBatch"));
      storage::ObDDLInsertDag *dag = nullptr;
      if (OB_FAIL(share::ObDDLUtil::get_data_information(*GCTX.sql_proxy_, param.ddl_task_id_,
                                                         task_data_info))) {
        LOG_WARN("fts aux direct get data information failed", K(ret));
      } else {
        dag_param.direct_load_type_ = storage::ObDDLDirectLoadUtil::ddl_get_direct_load_type();
        dag_param.ddl_thread_count_ = slice_cnt;
        dag_param.px_thread_count_ = 1;
        dag_param.ddl_task_param_.ddl_task_id_ = param.ddl_task_id_;
        dag_param.ddl_task_param_.execution_id_ = param.execution_id_;
        dag_param.ddl_task_param_.data_format_version_ = task_data_info.data_format_version_;
        dag_param.ddl_task_param_.snapshot_version_ = task_data_info.snapshot_version_;
        dag_param.ddl_task_param_.target_table_id_ = param.aux_table_id_;
        dag_param.ddl_task_param_.schema_version_ = task_data_info.schema_version_;
        dag_param.ddl_task_param_.is_offline_index_rebuild_ =
            task_data_info.is_offline_index_rebuild_;
      }
      if (OB_FAIL(ret)) {
      } else if (OB_FAIL(dag_param.tablet_ids_.assign(tablet_ids))) {
      } else if (OB_FAIL(share::ObDagScheduler::alloc_dag(dag_alloc, false, dag))) {
      } else if (OB_ISNULL(dag)) {
        ret = OB_ERR_UNEXPECTED;
      } else if (OB_FAIL(dag->init(&dag_param, nullptr, true /* add trace id */))) {
      } else {
        const int64_t t0 = common::ObTimeUtility::current_time();
        int tmp_ret = OB_SUCCESS;
        for (int64_t i = 0; OB_SUCC(ret) && i < tablet_ids.count(); ++i) {
          direct_wrote = true;
          if (OB_FAIL(write_fts_aux_rows(dag, tablet_ids.at(i), rows, batch_alloc))) {
            LOG_WARN("fts aux direct write failed", K(ret), "tablet_id", tablet_ids.at(i));
          }
        }
        if (OB_SUCC(ret)) {
          dag->set_px_finished();
          // Drive the remaining merge/checksum tasks of the insert DAG.
          tmp_ret = dag->process();
          if (OB_SUCCESS != tmp_ret && OB_ITER_END != tmp_ret) {
            ret = tmp_ret;
            LOG_WARN("fts aux direct dag process failed", K(ret));
          }
        }
        if (OB_SUCC(ret)) {
          ret = dag->get_dag_ret();
          if (OB_SUCC(ret)) {
            LOG_INFO("fts aux direct build done", K(param), K(doc_cnt), K(token_cnt),
                     K(rows.count()), "cost_us", common::ObTimeUtility::current_time() - t0);
          } else {
            LOG_WARN("fts aux direct dag failed", K(ret), K(param));
          }
        }
      }
    }
  }
  if (OB_FAIL(ret) && fts_aux_direct_enabled() && !direct_wrote) {
    // Nothing has been written yet: let the caller use the stock inner SQL.
    LOG_INFO("fts aux direct build unavailable, falling back", K(ret), K(param));
    ret = OB_NOT_SUPPORTED;
  }
  if (OB_SUCC(ret)) {
    LOG_INFO("fts aux scan build finished", K(param),
             "cost_us", common::ObTimeUtility::current_time() - start_time,
             "direct_build_enabled", fts_aux_direct_enabled());
    if (!fts_aux_direct_enabled()) {
      ret = OB_NOT_SUPPORTED;
    }
  }
  return ret;
}

} // namespace storage
} // namespace oceanbase
