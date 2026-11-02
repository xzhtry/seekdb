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

#include "share/ai_service/ob_ai_service_struct.h"
#include "lib/json/ob_json.h"


#define USING_LOG_PREFIX SHARE

using namespace oceanbase::share;
using namespace oceanbase::common;
using namespace oceanbase::json;

namespace oceanbase
{
namespace share
{
const ObString ObAiModelEndpointInfo::DEFAULT_SCOPE = "ALL";

static constexpr char OLLAMA_PROVIDER[] = "OLLAMA";

const char *VALID_PROVIDERS[] = {
  "ALIYUN-OPENAI",
  "ALIYUN-DASHSCOPE",
  "DEEPSEEK",
  "SILICONFLOW",
  "COHERE",
  "HUNYUAN-OPENAI",
  "OPENAI",
  "OLLAMA"
};

#define EXTRACT_JSON_ELEM_STR(json_key, member) \
  EXTRACT_JSON_ELEM_STR_WITH_PROCESS(json_key, member, "void")

#define EXTRACT_JSON_ELEM_STR_WITH_PROCESS(json_key, member, post_process) \
      if (elem.first.case_compare(json_key) == 0) { \
        if (elem.second->json_type() != ObJsonNodeType::J_STRING) { \
          ret = OB_AI_FUNC_PARAM_VALUE_INVALID; \
          LOG_USER_ERROR(OB_AI_FUNC_PARAM_VALUE_INVALID, elem.first.length(), elem.first.ptr()); \
          LOG_WARN("invalid json type", K(ret), K(elem.first), K(elem.second->json_type())); \
        } else { \
          member = ObString(elem.second->get_data_length(), elem.second->get_data()); \
          post_process; \
        } \
      } else

#define EXTRACT_JSON_ELEM_END() \
  { \
    ret = OB_AI_FUNC_PARAM_INVALID; \
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_INVALID, elem.first.length(), elem.first.ptr()); \
    LOG_WARN("unknown json key param", K(ret), K(elem.first)); \
  }

int ObAiModelEndpointInfo::parse_from_json_base(common::ObArenaAllocator &allocator, const ObString &name, const ObIJsonBase &params_jbase)
{
  int ret = OB_SUCCESS;
  reset();
  name_ = name;
  if (OB_FAIL(merge_delta_endpoint(allocator, params_jbase))) {
  }
  LOG_INFO("parse from json base", K(ret), K(params_jbase), K(params_jbase.json_type()), K(params_jbase.element_count()));
  return ret;
}

int ObAiModelEndpointInfo::deep_copy(
    common::ObIAllocator &allocator,
    const ObAiModelEndpointInfo &other)
{
  int ret = OB_SUCCESS;
  reset();
  endpoint_id_ = other.endpoint_id_;
  if (OB_FAIL(ob_write_string(allocator, other.name_, name_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.scope_, scope_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.ai_model_name_, ai_model_name_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.url_, url_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.access_key_, access_key_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.provider_, provider_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.request_model_name_, request_model_name_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.parameters_, parameters_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.request_transform_fn_, request_transform_fn_))) {
  } else if (OB_FAIL(ob_write_string(allocator, other.response_transform_fn_, response_transform_fn_))) {
  }
  return ret;
}

int ObAiModelEndpointInfo::check_valid() const
{
  int ret = OB_SUCCESS;
  if (name_.empty()) {
    ret = OB_AI_FUNC_PARAM_EMPTY;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_EMPTY, strlen("name"), "name");
  } else if (scope_.empty()) {
    ret = OB_AI_FUNC_PARAM_EMPTY;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_EMPTY, strlen("scope"), "scope");
  } else if (scope_.case_compare(DEFAULT_SCOPE) != 0) {
    ret = OB_AI_FUNC_PARAM_VALUE_INVALID;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_VALUE_INVALID, strlen("scope"), "scope");
  } else if (ai_model_name_.empty()) {
    ret = OB_AI_FUNC_PARAM_EMPTY;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_EMPTY, strlen("ai_model_name"), "ai_model_name");
  } else if (url_.empty()) {
    ret = OB_AI_FUNC_PARAM_EMPTY;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_EMPTY, strlen("url"), "url");
  } else if (access_key_.empty() && provider_.case_compare(OLLAMA_PROVIDER) != 0) {
    ret = OB_AI_FUNC_PARAM_EMPTY;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_EMPTY, strlen("access_key"), "access_key");
    LOG_WARN("access_key is empty", K(ret), K(*this));
  } else if (provider_.empty()) {
    ret = OB_AI_FUNC_PARAM_EMPTY;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_EMPTY, strlen("provider"), "provider");
  } else if (!is_valid_provider(provider_)) {
    ret = OB_AI_FUNC_PARAM_VALUE_INVALID;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_VALUE_INVALID, strlen("provider"), "provider");
  } else if (!parameters_.empty()) {
    ret = OB_AI_FUNC_PARAM_VALUE_INVALID;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_VALUE_INVALID, strlen("parameters"), "parameters");
  } else if (!request_transform_fn_.empty()) {
    ret = OB_AI_FUNC_PARAM_VALUE_INVALID;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_VALUE_INVALID, strlen("request_transform_fn"), "request_transform_fn");
  } else if (!response_transform_fn_.empty()) {
    ret = OB_AI_FUNC_PARAM_VALUE_INVALID;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_VALUE_INVALID, strlen("response_transform_fn"), "response_transform_fn");
  }
  return ret;
}

bool ObAiModelEndpointInfo::is_valid_provider(const ObString &provider)
{
  bool is_valid = false;
  for (int i = 0; i < ARRAYSIZEOF(VALID_PROVIDERS); i++) {
    if (provider.case_compare(VALID_PROVIDERS[i]) == 0) {
      is_valid = true;
      break;
    }
  }
  return is_valid;
}

int ObAiModelEndpointInfo::merge_delta_endpoint(common::ObArenaAllocator &allocator, const ObIJsonBase &delta_jbase)
{
  int ret = OB_SUCCESS;
  JsonObjectIterator iter = delta_jbase.object_iterator();
  bool has_api_key = false;
  while (!iter.end() && OB_SUCC(ret)) {
    ObJsonObjPair elem;
    if (OB_FAIL(iter.get_elem(elem))) {
    } else {
      EXTRACT_JSON_ELEM_STR("scope", scope_)
      EXTRACT_JSON_ELEM_STR("ai_model_name", ai_model_name_)
      EXTRACT_JSON_ELEM_STR("url", url_)
      EXTRACT_JSON_ELEM_STR_WITH_PROCESS("access_key", access_key_, has_api_key = true)
      EXTRACT_JSON_ELEM_STR("provider", provider_)
      EXTRACT_JSON_ELEM_STR("request_model_name", request_model_name_)
      EXTRACT_JSON_ELEM_STR("parameters", parameters_)
      EXTRACT_JSON_ELEM_STR("request_transform_fn", request_transform_fn_)
      EXTRACT_JSON_ELEM_STR("response_transform_fn", response_transform_fn_)
      EXTRACT_JSON_ELEM_END()
    }
    iter.next();
  }

  if (OB_SUCC(ret)) {
    if (has_api_key && !access_key_.empty() && OB_FAIL(encrypt_access_key_(allocator, access_key_, access_key_))) {
      LOG_WARN("failed to encrypt access key", K(ret));
    } else if (OB_FAIL(check_valid())) {
    }
  }

  LOG_INFO("merge delta endpoint", K(ret), K(delta_jbase), K(delta_jbase.json_type()), K(delta_jbase.element_count()));
  return ret;
}

int ObAiModelEndpointInfo::encrypt_access_key_(ObIAllocator &allocator, const ObString &access_key, ObString &encrypted_access_key)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ob_write_string(allocator, access_key, encrypted_access_key))) {
  }
  return ret;
}

int ObAiModelEndpointInfo::decrypt_access_key_(ObIAllocator &allocator, const ObString &encrypted_access_key, ObString &unencrypted_access_key) const
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(ob_write_string(allocator, encrypted_access_key, unencrypted_access_key))) {
  }
  return ret;
}

const char *EndpointType::ENDPOINT_TYPE_STR[] = {
  "DENSE_EMBEDDING",
  "SPARSE_EMBEDDING",
  "COMPLETION",
  "RERANK",
};

EndpointType::TYPE EndpointType::str_to_endpoint_type(const ObString &type_str)
{
  STATIC_ASSERT(static_cast<int64_t>(EndpointType::MAX_TYPE) == ARRAYSIZEOF(ENDPOINT_TYPE_STR) + 1, "endpoint type str len is mismatch");
  EndpointType::TYPE endpoint_type = EndpointType::INVALID_TYPE;
  bool is_found = false;
  for (int i = 1; i < EndpointType::MAX_TYPE && !is_found; i++) {
    if (type_str.case_compare(ENDPOINT_TYPE_STR[i-1]) == 0) {
      endpoint_type = static_cast<EndpointType::TYPE>(i);
      is_found = true;
    }
  }
  return endpoint_type;
}

int ObAiModelEndpointInfo::get_unencrypted_access_key(common::ObIAllocator &allocator, ObString &unencrypted_access_key) const
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(decrypt_access_key_(allocator, access_key_, unencrypted_access_key))) {
  }
  return ret;
}

int ObAiServiceModelInfo::parse_from_json_base(const ObString &name, const common::ObIJsonBase &params_jbase)
{
  int ret = OB_SUCCESS;
  reset();
  name_ = name;
  JsonObjectIterator iter = params_jbase.object_iterator();
  ObString type_str;
  while (!iter.end() && OB_SUCC(ret)) {
    ObJsonObjPair elem;
    if (OB_FAIL(iter.get_elem(elem))) {
    } else {
      EXTRACT_JSON_ELEM_STR("model_name", model_name_)
      EXTRACT_JSON_ELEM_STR_WITH_PROCESS("type", type_str, type_ = EndpointType::str_to_endpoint_type(type_str))
      EXTRACT_JSON_ELEM_END()
    }
    iter.next();
  }

  if (OB_SUCC(ret)) {
    if (OB_FAIL(check_valid())) {
    }
  }

  LOG_TRACE("parse from json base", K(ret), K(params_jbase), K(params_jbase.json_type()), K(params_jbase.element_count()));
  return ret;
}

int ObAiServiceModelInfo::check_valid() const
{
  int ret = OB_SUCCESS;
  if (name_.empty()) {
    ret = OB_AI_FUNC_PARAM_EMPTY;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_EMPTY, strlen("name"), "name");
  } else if (model_name_.empty()) {
    ret = OB_AI_FUNC_PARAM_EMPTY;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_EMPTY, strlen("model_name"), "model_name");
  } else if (type_ == EndpointType::MAX_TYPE) {
    ret = OB_AI_FUNC_PARAM_EMPTY;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_EMPTY, strlen("type"), "type");
  } else if (type_ == EndpointType::INVALID_TYPE) {
    ret = OB_AI_FUNC_PARAM_VALUE_INVALID;
    LOG_USER_ERROR(OB_AI_FUNC_PARAM_VALUE_INVALID, strlen("type"), "type");
  }
  return ret;
}

OB_SERIALIZE_MEMBER(ObAiServiceModelInfo, name_, type_, model_name_);

} // namespace share
} // namespace oceanbase

#include <pthread.h>
#include <stdarg.h>
#include <cstdio>
#include <string>
#include <unistd.h>
#include "share/ob_server_struct.h"
#include "lib/string/ob_sql_string.h"
#include "common/mysqlclient/ob_mysql_proxy.h"
#include "common/mysqlclient/ob_single_connection_proxy.h"
#include "common/mysqlclient/ob_mysql_result.h"

namespace oceanbase
{
namespace share
{

static volatile bool g_ai_col_worker_stop = false;

static void ai_worker_log(const char *fmt, ...)
{
  FILE *fp = fopen("/tmp/ai_worker.log", "a");
  if (NULL != fp) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
    fprintf(fp, "\n");
    fclose(fp);
  }
}

static bool g_ai_col_worker_started = false;
static pthread_t g_ai_col_worker_tid;

// 转义单引号与反斜杠，拼进 SQL 字符串字面量
static void escape_sql_string(const ObString &in, ObSqlString &out)
{
  for (int64_t i = 0; i < in.length(); ++i) {
    const char c = in.ptr()[i];
    if ('\'' == c) {
      out.append("''");
    } else if ('\\' == c) {
      out.append("\\\\");
    } else {
      out.append(&c, 1);
    }
  }
}

// 执行一条“取单个字符串结果”的 SQL
static int query_one_value(const char *sql, ObString &value, ObArenaAllocator &allocator)
{
  int ret = OB_SUCCESS;
  ObSingleConnectionProxy conn_proxy;
  if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(conn_proxy.connect(0, GCTX.sql_proxy_))) {
  } else {
  SMART_VAR(common::ObMySQLProxy::MySQLResult, res) {
    common::sqlclient::ObMySQLResult *result = nullptr;
    ObString whole;
    if (OB_FAIL(conn_proxy.read(res, sql, 0))) {
    } else if (OB_ISNULL(result = res.get_result())) {
      ret = OB_ERR_UNEXPECTED;
    } else if (OB_FAIL(result->next())) {
      if (OB_ITER_END == ret) {
        ret = OB_SUCCESS;
        value.reset();
      }
    } else if (OB_FAIL(result->get_varchar("v", whole))) {
    } else if (OB_FAIL(ob_write_string(allocator, whole, value))) {
    }
  }
  }
  return ret;
}

static int exec_dml(const char *sql)
{
  int ret = OB_SUCCESS;
  int64_t affected_rows = 0;
  ObSingleConnectionProxy conn_proxy;
  if (OB_ISNULL(GCTX.sql_proxy_)) {
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(conn_proxy.connect(0, GCTX.sql_proxy_))) {
  } else if (OB_FAIL(conn_proxy.write(sql, affected_rows))) {
  }
  return ret;
}

static void process_one_task(ObArenaAllocator &allocator,
                             const ObString &db_name,
                             const ObString &table_name,
                             const ObString &column_name,
                             const ObString &predicate,
                             const ObString &expr,
                             int64_t retry_count,
                             int64_t max_retries)
{
  int ret = OB_SUCCESS;
  ObSqlString sql;
  ObString result;
  // 去掉 JSON 列的提示词后缀，按原始表达式求值
  ObString raw_expr = expr;
  static const char *SUFFIX = " Return exactly one valid JSON value.";
  const int64_t slen = static_cast<int64_t>(STRLEN(SUFFIX));
  if (raw_expr.length() > slen &&
      0 == MEMCMP(raw_expr.ptr() + raw_expr.length() - slen, SUFFIX, slen)) {
    raw_expr.assign_ptr(raw_expr.ptr(), static_cast<int32_t>(raw_expr.length() - slen));
  }
  ai_worker_log("task begin db=%s tbl=%s col=%s pred=%s retry=%ld",
                std::string(db_name.ptr(), db_name.length()).c_str(),
                std::string(table_name.ptr(), table_name.length()).c_str(),
                std::string(column_name.ptr(), column_name.length()).c_str(),
                std::string(predicate.ptr(), predicate.length()).c_str(), retry_count);
  if (OB_FAIL(sql.assign_fmt("SELECT %.*s AS v FROM `%.*s`.`%.*s` WHERE %.*s",
                             raw_expr.length(), raw_expr.ptr(),
                             db_name.length(), db_name.ptr(),
                             table_name.length(), table_name.ptr(),
                             predicate.length(), predicate.ptr()))) {
  } else if (OB_FAIL(query_one_value(sql.ptr(), result, allocator))) {
    ai_worker_log("select failed ret=%d sql=%s", ret, sql.ptr());
  } else if (result.empty()) {
    ai_worker_log("select empty sql=%s", sql.ptr());
    ret = OB_ERR_UNEXPECTED;
  } else {
    ObSqlString update;
    ObSqlString value;
    escape_sql_string(result, value);
    if (OB_FAIL(update.assign_fmt("UPDATE `%.*s`.`%.*s` SET `%.*s` = '%.*s' WHERE %.*s",
                                  db_name.length(), db_name.ptr(),
                                  table_name.length(), table_name.ptr(),
                                  column_name.length(), column_name.ptr(),
                                  value.length(), value.ptr(),
                                  predicate.length(), predicate.ptr()))) {
    } else if (OB_FAIL(exec_dml(update.ptr()))) {
      ai_worker_log("update failed ret=%d sql=%s", ret, update.ptr());
    } else {
    }
  }
  // 清理/重试
  ObSqlString clean;
  if (OB_SUCC(ret)) {
    clean.assign_fmt("DELETE FROM oceanbase.__all_ai_column_task WHERE rowkey_predicate = '%.*s' "
                     "AND column_id = (SELECT column_id FROM oceanbase.__all_column WHERE table_id = "
                     "(SELECT table_id FROM oceanbase.__all_table WHERE table_name = '%.*s') AND column_name = '%.*s')",
                     predicate.length(), predicate.ptr(),
                     table_name.length(), table_name.ptr(),
                     column_name.length(), column_name.ptr());
  } else if (retry_count + 1 > max_retries) {
    clean.assign_fmt("DELETE FROM oceanbase.__all_ai_column_task WHERE rowkey_predicate = '%.*s' AND column_id = "
                     "(SELECT column_id FROM oceanbase.__all_column WHERE table_id = "
                     "(SELECT table_id FROM oceanbase.__all_table WHERE table_name = '%.*s') AND column_name = '%.*s')",
                     predicate.length(), predicate.ptr(),
                     table_name.length(), table_name.ptr(),
                     column_name.length(), column_name.ptr());
  } else {
    clean.assign_fmt("UPDATE oceanbase.__all_ai_column_task SET retry_count = retry_count + 1, "
                     "eligible_ts = unix_timestamp() + 1 WHERE rowkey_predicate = '%.*s' AND column_id = "
                     "(SELECT column_id FROM oceanbase.__all_column WHERE table_id = "
                     "(SELECT table_id FROM oceanbase.__all_table WHERE table_name = '%.*s') AND column_name = '%.*s')",
                     predicate.length(), predicate.ptr(),
                     table_name.length(), table_name.ptr(),
                     column_name.length(), column_name.ptr());
  }
  if (clean.length() > 0) {
    int tmp_ret = exec_dml(clean.ptr());
  }
}

static void process_ai_column_tasks()
{
  int ret = OB_SUCCESS;
  ObArenaAllocator allocator("AiColWorker");
  const char *task_sql =
      "SELECT d.database_name, t.table_name, c.column_name, a.rowkey_predicate, m.normalized_expr, "
      "a.retry_count "
      "FROM oceanbase.__all_ai_column_task a "
      "JOIN oceanbase.__all_table t ON t.table_id = a.table_id "
      "JOIN oceanbase.__all_database d ON d.database_id = t.database_id "
      "JOIN oceanbase.__all_column c ON c.table_id = a.table_id AND c.column_id = a.column_id "
      "JOIN oceanbase.__all_ai_column m ON m.table_id = a.table_id AND m.column_id = a.column_id "
      "WHERE a.state = 0 AND a.eligible_ts <= unix_timestamp() LIMIT 4";
  SMART_VAR(common::ObMySQLProxy::MySQLResult, res) {
    common::sqlclient::ObMySQLResult *result = nullptr;
    if (OB_ISNULL(GCTX.sql_proxy_)) {
      // do nothing
    } else if (OB_FAIL(GCTX.sql_proxy_->read(res, task_sql, 0))) {
      ai_worker_log("task query failed ret=%d", ret);
    } else if (OB_ISNULL(result = res.get_result())) {
    } else {
      ai_worker_log("round: reading tasks");
      while (OB_SUCC(result->next())) {
        ObString db_name;
        ObString table_name;
        ObString column_name;
        ObString predicate;
        ObString expr;
        int64_t retry_count = 0;
        if (OB_FAIL(result->get_varchar("database_name", db_name))) {
          break;
        } else if (OB_FAIL(result->get_varchar("table_name", table_name))) {
          break;
        } else if (OB_FAIL(result->get_varchar("column_name", column_name))) {
          break;
        } else if (OB_FAIL(result->get_varchar("rowkey_predicate", predicate))) {
          break;
        } else if (OB_FAIL(result->get_varchar("normalized_expr", expr))) {
          break;
        } else if (OB_FAIL(result->get_int("retry_count", retry_count))) {
          break;
        }
        process_one_task(allocator, db_name, table_name, column_name, predicate, expr,
                         retry_count, 3);
        allocator.reuse();
      }
    }
  }
}

static void *ai_column_worker_main(void *arg)
{
  UNUSED(arg);
  while (!g_ai_col_worker_stop) {
    process_ai_column_tasks();
    usleep(200 * 1000);
  }
  return nullptr;
}

int start_ai_column_worker()
{
  int ret = OB_SUCCESS;
  if (!g_ai_col_worker_started) {
    g_ai_col_worker_started = true;
    if (0 != pthread_create(&g_ai_col_worker_tid, nullptr, ai_column_worker_main, nullptr)) {
      ret = OB_ERR_UNEXPECTED;
      g_ai_col_worker_started = false;
    }
  }
  return ret;
}

}  // namespace share
}  // namespace oceanbase
