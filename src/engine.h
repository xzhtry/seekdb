#pragma once

#include "mvdb.h"
#include "sql.h"

namespace mvdb {

struct RowRef {
  u64 offset = 0;
  u32 length = 0;  // payload length
};

struct ResultSet {
  bool query = false;
  std::vector<std::string> columns;
  std::vector<std::vector<std::string>> rows;
  i64 affected = 0;
};

// ---------------------------------------------------------------------------
// Memory mapped read view of a table data file.
// ---------------------------------------------------------------------------
class MappedFile {
 public:
  MappedFile() = default;
  ~MappedFile();
  MappedFile(const MappedFile &) = delete;
  MappedFile &operator=(const MappedFile &) = delete;

  void set_fd(int fd) { fd_ = fd; }
  void remap(u64 size);
  void unmap();
  const char *data() const { return static_cast<const char *>(base_); }
  u64 size() const { return size_; }
  bool mapped() const { return base_ != nullptr; }

 private:
  int fd_ = -1;
  void *base_ = nullptr;
  u64 size_ = 0;
};

// ---------------------------------------------------------------------------
// WAL.
// ---------------------------------------------------------------------------
enum class WalType : u8 { ROW = 1, DEL = 2, COMMIT = 3, ABORT = 4, CHECKPOINT = 5 };

struct WalScan {
  bool empty = true;
  u64 max_txn = 0;
  u64 valid_bytes = 0;
  std::unordered_set<u64> committed;
  std::unordered_set<u64> aborted;
};

class Wal {
 public:
  Wal() = default;
  ~Wal();
  Wal(const Wal &) = delete;
  Wal &operator=(const Wal &) = delete;

  void open(const std::string &path);
  void close();
  u64 size() const;
  void append(WalType type, u64 txn, u64 row_id, u32 table_id, std::string_view payload,
              bool sync);
  void append_batch(std::string_view data, bool sync);
  WalScan scan();
  // Replay committed row/del records.  The callback receives type, txn, row_id, payload.
  void replay(const std::unordered_set<u64> &committed,
              const std::function<void(WalType, u64, u64, u32, const char *, u32)> &fn);
  void truncate();

 private:
  int fd_ = -1;
  std::string path_;
  u64 end_ = 0;
};

// ---------------------------------------------------------------------------
// Table.
// ---------------------------------------------------------------------------
class Database;

class Table {
 public:
  Table(Database *db, TableSchema schema, std::string data_path);
  ~Table();
  Table(const Table &) = delete;
  Table &operator=(const Table &) = delete;

  const TableSchema &schema() const { return schema_; }
  TableSchema &schema() { return schema_; }
  const std::string &name() const { return schema_.name; }

  void open_data();
  void close_data();

  // Scan the data file, validate records, and rebuild the committed row index.
  // Returns true if any record was not committed (needs compaction).
  bool recover_records(const WalScan &wal, u64 checkpoint, bool wal_empty);
  void apply_record(WalType type, u64 txn, i64 row_id, const char *payload, u32 len,
                    bool cache_it = true);
  RowRef append_record(WalType type, u64 txn, i64 row_id, std::string_view payload);
  void fsync_data();

  bool get_ref(i64 id, RowRef &ref) const;
  bool has_row(i64 id) const;
  std::optional<Row> load_row(const RowRef &ref) const;
  std::optional<Row> load_row_by_id(i64 id) const;
  void put_row(i64 id, Row row);
  void erase_row(i64 id);
  void cache_row(i64 id, const Row &row, bool force = false);
  const Row *cached(i64 id) const;

  void compact();
  void ensure_mapped();
  u64 live_bytes() const { return live_bytes_; }
  u64 data_bytes() const;
  void adjust_live(i64 delta) {
    if (delta < 0 && live_bytes_ < static_cast<u64>(-delta)) live_bytes_ = 0;
    else live_bytes_ = static_cast<u64>(static_cast<i64>(live_bytes_) + delta);
  }

  const std::unordered_map<i64, RowRef> &rows() const { return rows_; }
  std::unordered_map<i64, RowRef> &rows() { return rows_; }

  struct ScanEntry {
    i64 id;
    const Row *row;
    const float *vec;
  };
  const std::vector<ScanEntry> *scan_snapshot();
  void invalidate_snapshot() { snapshot_valid_ = false; }

 private:
  Database *db_;
  TableSchema schema_;
  std::string data_path_;
  int fd_ = -1;
  u64 file_size_ = 0;
  MappedFile map_;
  std::unordered_map<i64, RowRef> rows_;
  std::unordered_map<i64, Row> cache_;
  size_t cache_bytes_ = 0;
  size_t cache_budget_ = kDefaultCacheBudget;
  u64 live_bytes_ = 0;
  std::vector<ScanEntry> snapshot_;
  bool snapshot_valid_ = false;
  bool snapshot_usable_ = false;

  friend class Database;
};

// ---------------------------------------------------------------------------
// Database.
// ---------------------------------------------------------------------------
struct TxnOverlay {
  std::unordered_map<i64, std::optional<Row>> rows;
};

class Database {
 public:
  explicit Database(std::string dir);
  ~Database();

  void open();
  void close();

  ResultSet execute(const Statement &stmt);

  Table *find_table(std::string_view name) const;
  Table *find_table_by_id(u32 id) const;
  const std::unordered_map<u32, std::unique_ptr<Table>> &tables() const { return tables_; }

  bool in_txn() const { return txn_active_; }
  const std::string &default_mode() const { return default_mode_; }

  // Called by the WAL replay path.
  void replay_op(WalType type, u64 txn, i64 row_id, u32 table_id, const char *payload,
                 u32 len);

 private:
  std::string dir_;
  std::string catalog_path_;
  std::string meta_path_;
  std::string wal_path_;
  std::unordered_map<u32, std::unique_ptr<Table>> tables_;
  std::unordered_map<std::string, u32> table_by_name_;
  Wal wal_;
  u64 checkpoint_ = 0;
  u64 next_txn_ = 1;
  u64 next_table_id_ = 1;
  bool txn_active_ = false;
  u64 txn_id_ = 0;
  std::unordered_map<Table *, TxnOverlay> overlay_;
  std::string default_mode_ = "exact";
  bool opened_ = false;
  std::unordered_set<Table *> wal_touched_;

  void load_meta();
  void save_meta();
  void load_catalog();
  void save_catalog();
  void recover();
  void checkpoint_and_truncate();
  void checkpoint_now();

  void begin_txn();
  void commit_txn();
  void rollback_txn();
  void ensure_no_txn_for_ddl();
  u64 alloc_txn() { return next_txn_++; }

  ResultSet exec_select(const Statement &stmt);
  ResultSet exec_insert(const Statement &stmt);
  ResultSet exec_update(const Statement &stmt);
  ResultSet exec_delete(const Statement &stmt);
  ResultSet exec_create_table(const Statement &stmt);
  ResultSet exec_create_index(const Statement &stmt);
  ResultSet exec_drop_table(const Statement &stmt);
  ResultSet exec_drop_index(const Statement &stmt);
  ResultSet exec_set(const Statement &stmt);
  ResultSet exec_show_tables();
  ResultSet exec_describe(const Statement &stmt);

  std::optional<Row> visible_row(Table *t, i64 id) const;
  const std::optional<Row> *overlay_row(Table *t, i64 id) const;
  void set_overlay(Table *t, i64 id, std::optional<Row> row);
  void clear_overlay();

  struct Change {
    Table *table;
    i64 id;
    bool deleted;
    Row row;
  };
  void commit_changes(std::vector<Change> &changes);
  void apply_change_to_memory(Table *t, const Change &c);
};

// ---------------------------------------------------------------------------
// Expression / predicate evaluation.
// ---------------------------------------------------------------------------
struct EvalContext {
  const TableSchema *schema = nullptr;
  const Row *row = nullptr;
  const std::unordered_map<std::string, Value> *aliases = nullptr;
};

void prepare_expr(Expr &e, const TableSchema &schema);
Value eval_expr(const Expr &e, const EvalContext &ctx);
bool eval_cond(const Cond &c, const EvalContext &ctx);
void validate_expr_columns(const Expr &e, const TableSchema &schema);
void validate_cond_columns(const Cond &c, const TableSchema &schema);

std::string serialize_row(const TableSchema &schema, const Row &row);
Row deserialize_row(const TableSchema &schema, const char *payload, u32 len);
Value coerce_for_column(const Value &v, const Column &col);
Value literal_value(const Expr &e);

// Query execution helpers.
bool eval_row_matches(const Cond *where, const TableSchema &schema, const Row &row,
                      const std::unordered_map<std::string, Value> *aliases = nullptr);
std::string result_column_name(const SelectItem &item, const TableSchema &schema);

}  // namespace mvdb
