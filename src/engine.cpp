#include "engine.h"

#include <cfloat>
#include <cmath>
#include <charconv>

#include <sys/mman.h>
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace mvdb {

// ===========================================================================
// Record encoding.  The data file and the WAL share a 36-byte header:
//   0 magic u32 | 4 kind u8 | 5 flags u8 | 6 reserved u16 |
//   8 txn u64 | 16 row_id u64 | 24 table_id u32 | 28 payload_len u32 |
//   32 crc u32 | 36 payload
// The crc covers bytes [0,32) (including the zeroed crc slot) and the payload.
// ===========================================================================
static constexpr size_t kHeaderSize = 36;

static void put_u32_at(char *p, u32 v) {
  p[0] = static_cast<char>(v & 0xff);
  p[1] = static_cast<char>((v >> 8) & 0xff);
  p[2] = static_cast<char>((v >> 16) & 0xff);
  p[3] = static_cast<char>((v >> 24) & 0xff);
}
static void put_u64_at(char *p, u64 v) {
  for (int i = 0; i < 8; ++i) p[i] = static_cast<char>((v >> (8 * i)) & 0xff);
}
static u32 get_u32_at(const char *p) { return get_u32(p); }
static u64 get_u64_at(const char *p) { return get_u64(p); }

static std::string encode_record(u32 magic, WalType type, u64 txn, i64 row_id, u32 table_id,
                                 std::string_view payload) {
  if (payload.size() > kMaxRecordPayload) fail(1105, "record too large");
  std::string out;
  out.resize(kHeaderSize + payload.size());
  char *p = out.data();
  put_u32_at(p, magic);
  p[4] = static_cast<char>(type);
  p[5] = 0;
  p[6] = 0;
  p[7] = 0;
  put_u64_at(p + 8, txn);
  put_u64_at(p + 16, static_cast<u64>(row_id));
  put_u32_at(p + 24, table_id);
  put_u32_at(p + 28, static_cast<u32>(payload.size()));
  put_u32_at(p + 32, 0);
  if (!payload.empty()) std::memcpy(p + kHeaderSize, payload.data(), payload.size());
  u32 crc = crc32(p, 32);
  crc = crc32_update(crc, payload.data(), payload.size());
  put_u32_at(p + 32, crc);
  return out;
}

struct RecordHeader {
  u32 magic = 0;
  WalType type = WalType::ROW;
  u64 txn = 0;
  i64 row_id = 0;
  u32 table_id = 0;
  u32 payload_len = 0;
  u32 crc = 0;
};

// Returns false when the header is not a valid record (partial tail / corrupt).
static bool decode_header(const char *p, RecordHeader &h, u32 expected_magic) {
  h.magic = get_u32_at(p);
  if (h.magic != expected_magic) return false;
  u8 type = static_cast<u8>(p[4]);
  if (type < 1 || type > 5) return false;
  h.type = static_cast<WalType>(type);
  h.txn = get_u64_at(p + 8);
  h.row_id = static_cast<i64>(get_u64_at(p + 16));
  h.table_id = get_u32_at(p + 24);
  h.payload_len = get_u32_at(p + 28);
  h.crc = get_u32_at(p + 32);
  if (h.payload_len > kMaxRecordPayload) return false;
  return true;
}

static bool verify_crc(const char *header, const char *payload, u32 payload_len) {
  u32 stored = get_u32_at(header + 32);
  u32 crc = crc32(header, 32);
  crc = crc32_update(crc, payload, payload_len);
  return crc == stored;
}

// ===========================================================================
// Row serialization.
// ===========================================================================
static void put_f32(std::string &out, float f) {
  u32 bits;
  std::memcpy(&bits, &f, 4);
  put_u32(out, bits);
}

std::string serialize_row(const TableSchema &schema, const Row &row) {
  if (row.values.size() != schema.columns.size()) fail(1105, "row column count mismatch");
  std::string out;
  out.reserve(64);
  for (size_t i = 0; i < schema.columns.size(); ++i) {
    const Column &c = schema.columns[i];
    const Value &v = row.values[i];
    switch (c.type) {
      case ColType::BIGINT:
        if (v.type != ValueType::INT) fail(1105, "row value is not BIGINT");
        put_u64(out, static_cast<u64>(v.i));
        break;
      case ColType::VARCHAR: {
        if (v.type != ValueType::STRING) fail(1105, "row value is not VARCHAR");
        put_u32(out, static_cast<u32>(v.s.size()));
        out.append(v.s);
        break;
      }
      case ColType::VECTOR: {
        if (v.type != ValueType::VECTOR || v.v.size() != c.param)
          fail(1105, "row value is not VECTOR");
        for (float f : v.v) put_f32(out, f);
        break;
      }
    }
  }
  return out;
}

Row deserialize_row(const TableSchema &schema, const char *payload, u32 len) {
  Row row;
  row.values.reserve(schema.columns.size());
  const char *p = payload;
  const char *end = payload + len;
  auto need = [&](size_t n) {
    if (static_cast<size_t>(end - p) < n) fail(1105, "corrupt row payload");
  };
  for (const Column &c : schema.columns) {
    switch (c.type) {
      case ColType::BIGINT: {
        need(8);
        row.values.push_back(Value::make_int(static_cast<i64>(get_u64_at(p))));
        p += 8;
        break;
      }
      case ColType::VARCHAR: {
        need(4);
        u32 n = get_u32_at(p);
        p += 4;
        need(n);
        row.values.push_back(Value::make_string(std::string(p, n)));
        p += n;
        break;
      }
      case ColType::VECTOR: {
        size_t n = static_cast<size_t>(c.param) * 4;
        need(n);
        std::vector<float> vec(c.param);
        for (u32 i = 0; i < c.param; ++i) {
          u32 bits = get_u32_at(p + 4 * i);
          std::memcpy(&vec[i], &bits, 4);
        }
        row.values.push_back(Value::make_vector(std::move(vec)));
        p += n;
        break;
      }
    }
  }
  if (p != end) fail(1105, "corrupt row payload length");
  return row;
}

// ===========================================================================
// MappedFile.
// ===========================================================================
MappedFile::~MappedFile() { unmap(); }

void MappedFile::unmap() {
  if (base_) {
    ::munmap(base_, size_);
    base_ = nullptr;
  }
  size_ = 0;
}

void MappedFile::remap(u64 size) {
  if (size == size_ && (base_ || size == 0)) return;
  unmap();
  if (size == 0) return;
  void *p = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd_, 0);
  if (p == MAP_FAILED) fail(1105, std::string("mmap failed: ") + std::strerror(errno));
  base_ = p;
  size_ = size;
  ::madvise(p, size, MADV_RANDOM);
}

// ===========================================================================
// WAL.
// ===========================================================================
Wal::~Wal() { close(); }

void Wal::open(const std::string &path) {
  path_ = path;
  fd_ = ::open(path.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd_ < 0) fail(1105, "cannot open WAL: " + std::string(std::strerror(errno)));
  end_ = file_size(fd_);
}

void Wal::close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

u64 Wal::size() const { return end_; }

void Wal::append_batch(std::string_view data, bool sync) {
  if (data.empty()) return;
  write_all(fd_, data.data(), data.size(), static_cast<off_t>(end_));
  end_ += data.size();
  if (sync) ::fdatasync(fd_);
}

void Wal::append(WalType type, u64 txn, u64 row_id, u32 table_id, std::string_view payload,
                 bool sync) {
  std::string rec = encode_record(kWalMagic, type, txn, static_cast<i64>(row_id), table_id, payload);
  append_batch(rec, sync);
}

WalScan Wal::scan() {
  WalScan out;
  out.valid_bytes = 0;
  u64 off = 0;
  u64 total = file_size(fd_);
  std::string payload;
  while (off + kHeaderSize <= total) {
    char header[kHeaderSize];
    read_all(fd_, header, kHeaderSize, static_cast<off_t>(off));
    RecordHeader h;
    if (!decode_header(header, h, kWalMagic)) break;
    if (off + kHeaderSize + h.payload_len > total) break;
    payload.resize(h.payload_len);
    if (h.payload_len) read_all(fd_, payload.data(), h.payload_len, static_cast<off_t>(off + kHeaderSize));
    if (!verify_crc(header, payload.data(), h.payload_len)) break;
    out.empty = false;
    out.max_txn = std::max(out.max_txn, h.txn);
    if (h.type == WalType::COMMIT) out.committed.insert(h.txn);
    else if (h.type == WalType::ABORT) out.aborted.insert(h.txn);
    off += kHeaderSize + h.payload_len;
    out.valid_bytes = off;
  }
  if (out.valid_bytes < total) {
    ::ftruncate(fd_, static_cast<off_t>(out.valid_bytes));
    end_ = out.valid_bytes;
  }
  return out;
}

void Wal::replay(const std::unordered_set<u64> &committed,
                 const std::function<void(WalType, u64, u64, u32, const char *, u32)> &fn) {
  u64 off = 0;
  u64 total = file_size(fd_);
  std::string payload;
  while (off + kHeaderSize <= total) {
    char header[kHeaderSize];
    read_all(fd_, header, kHeaderSize, static_cast<off_t>(off));
    RecordHeader h;
    if (!decode_header(header, h, kWalMagic)) break;
    if (off + kHeaderSize + h.payload_len > total) break;
    payload.resize(h.payload_len);
    if (h.payload_len)
      read_all(fd_, payload.data(), h.payload_len, static_cast<off_t>(off + kHeaderSize));
    if (!verify_crc(header, payload.data(), h.payload_len)) break;
    if ((h.type == WalType::ROW || h.type == WalType::DEL) && committed.count(h.txn)) {
      fn(h.type, h.txn, static_cast<u64>(h.row_id), h.table_id, payload.data(), h.payload_len);
    }
    off += kHeaderSize + h.payload_len;
  }
}

void Wal::truncate() {
  if (fd_ < 0) return;
  ::ftruncate(fd_, 0);
  end_ = 0;
  ::fdatasync(fd_);
}

// ===========================================================================
// Table.
// ===========================================================================
Table::Table(Database *db, TableSchema schema, std::string data_path)
    : db_(db), schema_(std::move(schema)), data_path_(std::move(data_path)) {
  if (const char *e = std::getenv("MVDB_CACHE_MB")) {
    long v = std::strtol(e, nullptr, 10);
    if (v > 0) cache_budget_ = static_cast<size_t>(v) * 1024 * 1024;
  }
}

Table::~Table() { close_data(); }

void Table::open_data() {
  fd_ = ::open(data_path_.c_str(), O_RDWR | O_CREAT, 0644);
  if (fd_ < 0) fail(1105, "cannot open table data: " + std::string(std::strerror(errno)));
  file_size_ = file_size(fd_);
  map_.set_fd(fd_);
}

void Table::close_data() {
  map_.unmap();
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

void Table::ensure_mapped() {
  u64 sz = file_size(fd_);
  if (sz != file_size_) file_size_ = sz;
  map_.remap(file_size_);
}

u64 Table::data_bytes() const { return file_size_; }

bool Table::get_ref(i64 id, RowRef &ref) const {
  auto it = rows_.find(id);
  if (it == rows_.end()) return false;
  ref = it->second;
  return true;
}

bool Table::has_row(i64 id) const { return rows_.count(id) != 0; }

std::optional<Row> Table::load_row(const RowRef &ref) const {
  if (ref.offset + kHeaderSize + ref.length > map_.size()) {
    const_cast<Table *>(this)->ensure_mapped();
  }
  if (ref.offset + kHeaderSize + ref.length > map_.size()) return std::nullopt;
  const char *p = map_.data() + ref.offset;
  RecordHeader h;
  if (!decode_header(p, h, kDataMagic)) return std::nullopt;
  if (h.payload_len != ref.length || !verify_crc(p, p + kHeaderSize, h.payload_len))
    return std::nullopt;
  return deserialize_row(schema_, p + kHeaderSize, h.payload_len);
}

std::optional<Row> Table::load_row_by_id(i64 id) const {
  auto it = rows_.find(id);
  if (it == rows_.end()) return std::nullopt;
  auto c = cache_.find(id);
  if (c != cache_.end()) return c->second;
  return load_row(it->second);
}

const Row *Table::cached(i64 id) const {
  auto it = cache_.find(id);
  return it == cache_.end() ? nullptr : &it->second;
}

const std::vector<Table::ScanEntry> *Table::scan_snapshot() {
  if (snapshot_valid_) return snapshot_usable_ ? &snapshot_ : nullptr;
  snapshot_valid_ = true;
  snapshot_usable_ = false;
  snapshot_.clear();
  if (cache_.size() != rows_.size()) return nullptr;
  snapshot_.reserve(rows_.size());
  int vc = schema_.vector_index();
  for (const auto &kv : rows_) {
    auto c = cache_.find(kv.first);
    if (c == cache_.end()) return nullptr;
    const float *vp = nullptr;
    if (vc >= 0 && static_cast<size_t>(vc) < c->second.values.size() &&
        c->second.values[vc].type == ValueType::VECTOR)
      vp = c->second.values[vc].v.data();
    snapshot_.push_back(ScanEntry{kv.first, &c->second, vp});
  }
  std::sort(snapshot_.begin(), snapshot_.end(),
            [](const ScanEntry &a, const ScanEntry &b) { return a.id < b.id; });
  snapshot_usable_ = true;
  return &snapshot_;
}

static size_t row_memory(const Row &r) {
  size_t n = sizeof(Row) + r.values.size() * sizeof(Value);
  for (const auto &v : r.values) {
    n += v.s.size();
    n += v.v.size() * sizeof(float);
  }
  return n;
}

void Table::cache_row(i64 id, const Row &row, bool force) {
  snapshot_valid_ = false;
  size_t need = row_memory(row);
  auto it = cache_.find(id);
  if (it != cache_.end()) {
    cache_bytes_ = cache_bytes_ > row_memory(it->second) ? cache_bytes_ - row_memory(it->second) : 0;
    it->second = row;
    cache_bytes_ += need;
    return;
  }
  if (!force && cache_bytes_ + need > cache_budget_) return;
  cache_.emplace(id, row);
  cache_bytes_ += need;
}

void Table::put_row(i64 id, Row row) {
  cache_row(id, row, false);
}

void Table::erase_row(i64 id) {
  snapshot_valid_ = false;
  rows_.erase(id);
  auto it = cache_.find(id);
  if (it != cache_.end()) {
    cache_bytes_ = cache_bytes_ > row_memory(it->second) ? cache_bytes_ - row_memory(it->second) : 0;
    cache_.erase(it);
  }
}

RowRef Table::append_record(WalType type, u64 txn, i64 row_id, std::string_view payload) {
  std::string rec = encode_record(kDataMagic, type, txn, row_id, schema_.id, payload);
  write_all(fd_, rec.data(), rec.size(), static_cast<off_t>(file_size_));
  RowRef ref;
  ref.offset = file_size_;
  ref.length = static_cast<u32>(payload.size());
  file_size_ += rec.size();
  return ref;
}

void Table::fsync_data() { ::fdatasync(fd_); }

bool Table::recover_records(const WalScan &wal, u64 checkpoint, bool wal_empty) {
  bool needs_compact = false;
  u64 off = 0;
  u64 total = file_size(fd_);
  std::string payload;
  while (off + kHeaderSize <= total) {
    char header[kHeaderSize];
    read_all(fd_, header, kHeaderSize, static_cast<off_t>(off));
    RecordHeader h;
    if (!decode_header(header, h, kDataMagic)) break;
    if (h.table_id != schema_.id && h.table_id != 0) break;
    if (off + kHeaderSize + h.payload_len > total) break;
    payload.resize(h.payload_len);
    if (h.payload_len)
      read_all(fd_, payload.data(), h.payload_len, static_cast<off_t>(off + kHeaderSize));
    if (!verify_crc(header, payload.data(), h.payload_len)) break;
    bool visible = false;
    if (wal_empty) {
      visible = (checkpoint == 0) || (h.txn <= checkpoint);
    } else {
      visible = (h.txn <= checkpoint) || wal.committed.count(h.txn) > 0;
    }
    RowRef ref{off, h.payload_len};
    if (visible) {
      if (h.type == WalType::ROW) {
        rows_[h.row_id] = ref;
        if (cache_.size() * 2 < rows_.size() + 16 || cache_bytes_ < cache_budget_) {
          Row row = deserialize_row(schema_, payload.data(), h.payload_len);
          cache_row(h.row_id, row);
        }
      } else if (h.type == WalType::DEL) {
        erase_row(h.row_id);
      }
    } else {
      needs_compact = true;
    }
    off += kHeaderSize + h.payload_len;
  }
  if (off < total) {
    // Partial tail from a crash.  It is not part of any committed record.
    ::ftruncate(fd_, static_cast<off_t>(off));
    file_size_ = off;
    needs_compact = true;
  }
  ensure_mapped();
  live_bytes_ = file_size_;
  snapshot_valid_ = false;
  return needs_compact;
}

void Table::compact() {
  ensure_mapped();
  std::string tmp = data_path_ + ".compact";
  int out = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (out < 0) fail(1105, "cannot create compaction file: " + std::string(std::strerror(errno)));
  std::vector<std::pair<i64, RowRef>> refs;
  refs.reserve(rows_.size());
  u64 off = 0;
  try {
    for (const auto &kv : rows_) {
      i64 id = kv.first;
      std::optional<Row> row;
      auto c = cache_.find(id);
      if (c != cache_.end()) row = c->second;
      else row = load_row(kv.second);
      if (!row) continue;
      std::string payload = serialize_row(schema_, *row);
      std::string rec = encode_record(kDataMagic, WalType::ROW, 0, id, schema_.id, payload);
      write_all(out, rec.data(), rec.size(), static_cast<off_t>(off));
      RowRef ref{off, static_cast<u32>(payload.size())};
      refs.emplace_back(id, ref);
      off += rec.size();
    }
    ::fdatasync(out);
  } catch (...) {
    ::close(out);
    ::unlink(tmp.c_str());
    throw;
  }
  ::close(out);
  if (::rename(tmp.c_str(), data_path_.c_str()) != 0) {
    ::unlink(tmp.c_str());
    fail(1105, "cannot rename compacted table: " + std::string(std::strerror(errno)));
  }
  fsync_dir(data_path_);
  close_data();
  open_data();
  rows_.clear();
  for (auto &r : refs) rows_[r.first] = r.second;
  live_bytes_ = off;
  snapshot_valid_ = false;
  ensure_mapped();
}

// ===========================================================================
// Catalog / meta.
// ===========================================================================
static void put_str(std::string &out, std::string_view s) {
  put_u32(out, static_cast<u32>(s.size()));
  out.append(s);
}

static bool get_str(const char *&p, const char *end, std::string &out) {
  if (end - p < 4) return false;
  u32 n = get_u32_at(p);
  p += 4;
  if (static_cast<size_t>(end - p) < n) return false;
  out.assign(p, n);
  p += n;
  return true;
}

static std::string serialize_catalog(const std::vector<TableSchema> &schemas) {
  std::string out;
  put_u32(out, kCatalogMagic);
  put_u32(out, static_cast<u32>(schemas.size()));
  for (const auto &s : schemas) {
    put_u32(out, s.id);
    put_str(out, s.name);
    put_u32(out, static_cast<u32>(s.columns.size()));
    put_u32(out, static_cast<u32>(s.pk_index));
    for (const auto &c : s.columns) {
      put_str(out, c.name);
      out.push_back(static_cast<char>(c.type));
      put_u32(out, c.param);
    }
    put_u32(out, static_cast<u32>(s.vector_indexes.size()));
    for (const auto &ix : s.vector_indexes) {
      put_str(out, ix.first);
      put_str(out, ix.second);
    }
  }
  u32 crc = crc32(out.data(), out.size());
  put_u32(out, crc);
  return out;
}

static bool parse_catalog(const std::string &content, std::vector<TableSchema> &out) {
  if (content.size() < 12) return false;
  u32 stored_crc = get_u32_at(content.data() + content.size() - 4);
  if (crc32(content.data(), content.size() - 4) != stored_crc) return false;
  const char *p = content.data();
  const char *end = content.data() + content.size() - 4;
  if (get_u32_at(p) != kCatalogMagic) return false;
  p += 4;
  u32 count = get_u32_at(p);
  p += 4;
  if (count > 100000) return false;
  for (u32 i = 0; i < count; ++i) {
    TableSchema s;
    if (end - p < 8) return false;
    s.id = get_u32_at(p);
    p += 4;
    if (!get_str(p, end, s.name)) return false;
    u32 ncols = get_u32_at(p);
    p += 4;
    s.pk_index = static_cast<int>(get_u32_at(p));
    p += 4;
    if (ncols > 10000) return false;
    for (u32 c = 0; c < ncols; ++c) {
      Column col;
      if (!get_str(p, end, col.name)) return false;
      if (p >= end) return false;
      col.type = static_cast<ColType>(static_cast<u8>(*p++));
      if (end - p < 4) return false;
      col.param = get_u32_at(p);
      p += 4;
      s.columns.push_back(std::move(col));
    }
    if (end - p < 4) return false;
    u32 nix = get_u32_at(p);
    p += 4;
    for (u32 k = 0; k < nix; ++k) {
      std::string a, b;
      if (!get_str(p, end, a) || !get_str(p, end, b)) return false;
      s.vector_indexes.emplace_back(std::move(a), std::move(b));
    }
    out.push_back(std::move(s));
  }
  return p == end;
}

Database::Database(std::string dir) : dir_(std::move(dir)) {
  catalog_path_ = dir_ + "/catalog.db";
  meta_path_ = dir_ + "/meta.db";
  wal_path_ = dir_ + "/wal.log";
}

Database::~Database() {
  for (auto &kv : tables_) kv.second->close_data();
  wal_.close();
}

void Database::load_meta() {
  checkpoint_ = 0;
  if (!path_exists(meta_path_)) return;
  int fd = ::open(meta_path_.c_str(), O_RDONLY);
  if (fd < 0) return;
  u64 sz = file_size(fd);
  if (sz >= 16) {
    char buf[16];
    read_all(fd, buf, 16, 0);
    if (get_u32_at(buf) == kMetaMagic) {
      u32 stored = get_u32_at(buf + 12);
      if (crc32(buf, 12) == stored) checkpoint_ = get_u64_at(buf + 4);
    }
  }
  ::close(fd);
}

void Database::save_meta() {
  std::string out;
  put_u32(out, kMetaMagic);
  put_u64(out, checkpoint_);
  u32 crc = crc32(out.data(), out.size());
  put_u32(out, crc);
  atomic_write_file(meta_path_, out);
}

void Database::load_catalog() {
  if (!path_exists(catalog_path_)) return;
  int fd = ::open(catalog_path_.c_str(), O_RDONLY);
  if (fd < 0) fail(1105, "cannot open catalog");
  u64 sz = file_size(fd);
  std::string content(static_cast<size_t>(sz), '\0');
  if (sz) read_all(fd, content.data(), static_cast<size_t>(sz), 0);
  ::close(fd);
  std::vector<TableSchema> schemas;
  if (!parse_catalog(content, schemas)) fail(1105, "corrupt catalog");
  for (auto &s : schemas) {
    TableSchema copy = s;
    std::string path = dir_ + "/tbl_" + std::to_string(s.id) + ".dat";
    auto t = std::make_unique<Table>(this, std::move(copy), path);
    t->open_data();
    tables_[s.id] = std::move(t);
    table_by_name_[to_lower(s.name)] = s.id;
    next_table_id_ = std::max(next_table_id_, static_cast<u64>(s.id) + 1);
  }
}

void Database::save_catalog() {
  std::vector<TableSchema> schemas;
  schemas.reserve(tables_.size());
  for (auto &kv : tables_) schemas.push_back(kv.second->schema());
  std::sort(schemas.begin(), schemas.end(),
            [](const TableSchema &a, const TableSchema &b) { return a.id < b.id; });
  atomic_write_file(catalog_path_, serialize_catalog(schemas));
}

void Database::replay_op(WalType type, u64 txn, i64 row_id, u32 table_id, const char *payload,
                         u32 len) {
  auto it = tables_.find(table_id);
  if (it == tables_.end()) return;
  Table *t = it->second.get();
  RowRef ref = t->append_record(type, txn, row_id, std::string_view(payload, len));
  if (type == WalType::ROW) {
    t->rows()[row_id] = ref;
    Row row = deserialize_row(t->schema(), payload, len);
    t->cache_row(row_id, row, false);
  } else {
    t->erase_row(row_id);
  }
  wal_touched_.insert(t);
}

void Database::recover() {
  load_meta();
  load_catalog();
  wal_.open(wal_path_);
  WalScan scan = wal_.scan();
  bool wal_empty = scan.empty;

  checkpoint_ = std::max(checkpoint_, static_cast<u64>(0));
  next_txn_ = std::max(checkpoint_, scan.max_txn) + 1;

  std::vector<Table *> need_compact;
  for (auto &kv : tables_) {
    Table *t = kv.second.get();
    if (t->recover_records(scan, checkpoint_, wal_empty)) need_compact.push_back(t);
  }

  // Replay committed WAL operations.  Some of them may already be in the data
  // files (a crash between the WAL fsync and the data write); duplicates are
  // harmless and are removed by the compaction below.
  if (!wal_empty && !scan.committed.empty()) {
    wal_.replay(scan.committed, [this](WalType type, u64 txn, u64 row_id, u32 table_id,
                                       const char *payload, u32 len) {
      this->replay_op(type, txn, static_cast<i64>(row_id), table_id, payload, len);
    });
  }
  for (Table *t : wal_touched_) need_compact.push_back(t);
  std::sort(need_compact.begin(), need_compact.end());
  need_compact.erase(std::unique(need_compact.begin(), need_compact.end()), need_compact.end());
  for (Table *t : need_compact) t->compact();

  // All records are now committed and durable enough to make the WAL
  // redundant.  Record the checkpoint first, then truncate the log.
  u64 max_seen = std::max(checkpoint_, scan.max_txn);
  for (auto &kv : tables_) {
    (void)kv;
  }
  checkpoint_ = max_seen;
  save_meta();
  wal_.truncate();
  next_txn_ = checkpoint_ + 1;
  wal_touched_.clear();
  for (auto &kv : tables_) kv.second->ensure_mapped();
}

void Database::checkpoint_and_truncate() {
  if (txn_active_) rollback_txn();
  for (auto &kv : tables_) {
    kv.second->fsync_data();
    kv.second->ensure_mapped();
  }
  checkpoint_ = std::max(checkpoint_, next_txn_ > 0 ? next_txn_ - 1 : 0);
  save_meta();
  wal_.truncate();
}

void Database::checkpoint_now() {
  for (auto &kv : tables_) {
    kv.second->fsync_data();
    kv.second->ensure_mapped();
  }
  checkpoint_ = std::max(checkpoint_, next_txn_ > 0 ? next_txn_ - 1 : 0);
  save_meta();
  wal_.truncate();
}

void Database::open() {
  if (opened_) return;
  make_dir(dir_);
  recover();
  for (auto &kv : tables_) kv.second->ensure_mapped();
  opened_ = true;
}

void Database::close() {
  if (!opened_) return;
  checkpoint_and_truncate();
  for (auto &kv : tables_) kv.second->close_data();
  wal_.close();
  opened_ = false;
}

Table *Database::find_table(std::string_view name) const {
  auto it = table_by_name_.find(to_lower(std::string(name)));
  if (it == table_by_name_.end()) return nullptr;
  auto t = tables_.find(it->second);
  return t == tables_.end() ? nullptr : t->second.get();
}

Table *Database::find_table_by_id(u32 id) const {
  auto it = tables_.find(id);
  return it == tables_.end() ? nullptr : it->second.get();
}

// ===========================================================================
// Transaction overlay.
// ===========================================================================
const std::optional<Row> *Database::overlay_row(Table *t, i64 id) const {
  auto o = overlay_.find(t);
  if (o == overlay_.end()) return nullptr;
  auto r = o->second.rows.find(id);
  if (r == o->second.rows.end()) return nullptr;
  return &r->second;
}

void Database::set_overlay(Table *t, i64 id, std::optional<Row> row) {
  overlay_[t].rows[id] = std::move(row);
}

void Database::clear_overlay() { overlay_.clear(); }

std::optional<Row> Database::visible_row(Table *t, i64 id) const {
  if (const auto *o = overlay_row(t, id)) return *o;
  return t->load_row_by_id(id);
}

void Database::begin_txn() {
  // MySQL semantics: a second BEGIN implicitly commits the previous
  // transaction and starts a new one.  Nested transactions are not supported.
  if (txn_active_) commit_txn();
  txn_active_ = true;
  txn_id_ = alloc_txn();
  overlay_.clear();
}

void Database::rollback_txn() {
  if (!txn_active_) return;
  txn_active_ = false;
  txn_id_ = 0;
  clear_overlay();
}

void Database::apply_change_to_memory(Table *t, const Change &c) {
  if (c.deleted) {
    t->erase_row(c.id);
  } else {
    t->put_row(c.id, c.row);
  }
}

void Database::commit_changes(std::vector<Change> &changes) {
  if (changes.empty()) return;
  u64 txn = alloc_txn();
  std::string walbuf;
  for (const Change &c : changes) {
    std::string payload;
    WalType type = WalType::DEL;
    if (!c.deleted) {
      payload = serialize_row(c.table->schema(), c.row);
      type = WalType::ROW;
    }
    std::string rec = encode_record(kWalMagic, type, txn, c.id, c.table->schema().id, payload);
    walbuf += rec;
  }
  walbuf += encode_record(kWalMagic, WalType::COMMIT, txn, 0, 0, {});
  wal_.append_batch(walbuf, true);

  for (Change &c : changes) {
    if (c.deleted) {
      c.table->append_record(WalType::DEL, txn, c.id, {});
      c.table->erase_row(c.id);
    } else {
      std::string payload = serialize_row(c.table->schema(), c.row);
      RowRef ref = c.table->append_record(WalType::ROW, txn, c.id, payload);
      c.table->rows()[c.id] = ref;
      c.table->cache_row(c.id, c.row, false);
    }
  }
  if (wal_.size() > 64ull * 1024ull * 1024ull) checkpoint_now();
}

void Database::commit_txn() {
  if (!txn_active_) return;
  std::vector<Change> changes;
  for (auto &kv : overlay_) {
    Table *t = kv.first;
    for (auto &r : kv.second.rows) {
      Change c;
      c.table = t;
      c.id = r.first;
      c.deleted = !r.second.has_value();
      if (!c.deleted) c.row = std::move(*r.second);
      changes.push_back(std::move(c));
    }
  }
  txn_active_ = false;
  u64 old_txn = txn_id_;
  txn_id_ = 0;
  clear_overlay();
  if (changes.empty()) {
    // Still emit a commit marker so the transaction id is not reused.
    wal_.append(WalType::COMMIT, old_txn, 0, 0, {}, true);
    if (wal_.size() > 64ull * 1024ull * 1024ull) checkpoint_now();
    return;
  }
  // commit_changes allocates a fresh id; preserve the begin-time id instead.
  // We inline the write here so that txn ids are stable and monotonic.
  u64 txn = old_txn;
  std::string walbuf;
  for (const Change &c : changes) {
    std::string payload;
    WalType type = WalType::DEL;
    if (!c.deleted) {
      payload = serialize_row(c.table->schema(), c.row);
      type = WalType::ROW;
    }
    walbuf += encode_record(kWalMagic, type, txn, c.id, c.table->schema().id, payload);
  }
  walbuf += encode_record(kWalMagic, WalType::COMMIT, txn, 0, 0, {});
  wal_.append_batch(walbuf, true);
  for (Change &c : changes) {
    if (c.deleted) {
      c.table->append_record(WalType::DEL, txn, c.id, {});
      c.table->erase_row(c.id);
    } else {
      std::string payload = serialize_row(c.table->schema(), c.row);
      RowRef ref = c.table->append_record(WalType::ROW, txn, c.id, payload);
      c.table->rows()[c.id] = ref;
      c.table->cache_row(c.id, c.row, false);
    }
  }
  if (wal_.size() > 64ull * 1024ull * 1024ull) checkpoint_now();
}

// ===========================================================================
// Expression evaluation.
// ===========================================================================
void validate_expr_columns(const Expr &e, const TableSchema &schema) {
  switch (e.kind) {
    case ExprKind::COLUMN:
      if (schema.find_column(e.s) < 0)
        fail(ER_BAD_FIELD, "Unknown column '" + e.s + "' in 'field list'");
      break;
    case ExprKind::L2_DISTANCE:
      if (e.args.size() != 2) fail(ER_PARSE, "L2_DISTANCE expects 2 arguments");
      validate_expr_columns(*e.args[0], schema);
      if (e.args[1]->kind == ExprKind::COLUMN) {
        if (schema.find_column(e.args[1]->s) < 0)
          fail(ER_BAD_FIELD, "Unknown column '" + e.args[1]->s + "' in 'field list'");
      }
      break;
    default:
      for (const auto &a : e.args) validate_expr_columns(*a, schema);
      break;
  }
}

void validate_cond_columns(const Cond &c, const TableSchema &schema) {
  if (c.kind == CondKind::COMPARE) {
    validate_expr_columns(*c.left, schema);
    validate_expr_columns(*c.right, schema);
  } else {
    for (const auto &k : c.children) validate_cond_columns(*k, schema);
  }
}

static void parse_query_vector(const Expr &arg, std::vector<double> &out) {
  if (arg.kind == ExprKind::VECTOR_LITERAL) {
    out = arg.nums;
    return;
  }
  if (arg.kind == ExprKind::STRING_LITERAL) {
    bool ok = false;
    out = parse_vector_text(arg.s, ok);
    if (!ok) fail(ER_INVALID_VECTOR, "invalid vector literal");
    return;
  }
  fail(ER_UNSUPPORTED, "L2_DISTANCE query must be a vector literal");
}

void prepare_expr(Expr &e, const TableSchema &schema) {
  switch (e.kind) {
    case ExprKind::COLUMN:
      if (schema.find_column(e.s) < 0)
        fail(ER_BAD_FIELD, "Unknown column '" + e.s + "' in 'field list'");
      break;
    case ExprKind::L2_DISTANCE: {
      if (e.args.size() != 2) fail(ER_PARSE, "L2_DISTANCE expects 2 arguments");
      prepare_expr(*e.args[0], schema);
      int vi = schema.vector_index();
      if (vi < 0) fail(ER_INVALID_VECTOR, "table has no VECTOR column");
      if (e.args[0]->kind != ExprKind::COLUMN)
        fail(ER_UNSUPPORTED, "L2_DISTANCE first argument must be a vector column");
      int ci = schema.find_column(e.args[0]->s);
      if (ci < 0 || schema.columns[ci].type != ColType::VECTOR)
        fail(ER_INVALID_VECTOR, "L2_DISTANCE first argument must be a VECTOR column");
      std::vector<double> q;
      parse_query_vector(*e.args[1], q);
      if (q.size() != schema.columns[ci].param)
        fail(ER_INVALID_VECTOR, "vector dimension mismatch");
      for (double d : q) {
        if (!std::isfinite(d) || std::fabs(d) > static_cast<double>(FLT_MAX))
          fail(ER_INVALID_VECTOR, "vector value out of range");
      }
      e.nums = std::move(q);
      e.fnums.resize(e.nums.size());
      for (size_t i = 0; i < e.nums.size(); ++i) e.fnums[i] = static_cast<float>(e.nums[i]);
      break;
    }
    default:
      for (auto &a : e.args) prepare_expr(*a, schema);
      break;
  }
}

static double l2_scalar(const float *a, const float *b, size_t n) {
  double sum = 0;
  for (size_t i = 0; i < n; ++i) {
    double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
    sum += d * d;
  }
  return std::sqrt(sum);
}

#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
__attribute__((target("avx2,fma"))) static double l2_avx2(const float *a, const float *b,
                                                          size_t n) {
  __m256d sum0 = _mm256_setzero_pd();
  __m256d sum1 = _mm256_setzero_pd();
  size_t i = 0;
  for (; i + 8 <= n; i += 8) {
    __m256 va = _mm256_loadu_ps(a + i);
    __m256 vb = _mm256_loadu_ps(b + i);
    __m256 d = _mm256_sub_ps(va, vb);
    __m128 lo = _mm256_castps256_ps128(d);
    __m128 hi = _mm256_extractf128_ps(d, 1);
    __m256d dlo = _mm256_cvtps_pd(lo);
    __m256d dhi = _mm256_cvtps_pd(hi);
    sum0 = _mm256_fmadd_pd(dlo, dlo, sum0);
    sum1 = _mm256_fmadd_pd(dhi, dhi, sum1);
  }
  double lanes0[4], lanes1[4];
  _mm256_storeu_pd(lanes0, sum0);
  _mm256_storeu_pd(lanes1, sum1);
  double total = 0;
  for (double v : lanes0) total += v;
  for (double v : lanes1) total += v;
  for (; i < n; ++i) {
    double d = static_cast<double>(a[i]) - static_cast<double>(b[i]);
    total += d * d;
  }
  return std::sqrt(total);
}
#endif

static double vector_l2_ptr(const float *a, const float *b, size_t n) {
#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
  static const bool have_avx2 =
      __builtin_cpu_supports("avx2") && __builtin_cpu_supports("fma");
  if (have_avx2 && n >= 16) return l2_avx2(a, b, n);
#endif
  return l2_scalar(a, b, n);
}

static double vector_l2(const std::vector<float> &a, const std::vector<float> &b) {
  if (a.size() != b.size()) fail(ER_INVALID_VECTOR, "vector dimension mismatch");
  return vector_l2_ptr(a.data(), b.data(), a.size());
}

Value eval_expr(const Expr &e, const EvalContext &ctx) {
  switch (e.kind) {
    case ExprKind::INT_LITERAL: return Value::make_int(e.i);
    case ExprKind::DOUBLE_LITERAL: return Value::make_double(e.d);
    case ExprKind::STRING_LITERAL: return Value::make_string(e.s);
    case ExprKind::VECTOR_LITERAL: {
      std::vector<float> v(e.nums.size());
      for (size_t i = 0; i < e.nums.size(); ++i) v[i] = static_cast<float>(e.nums[i]);
      return Value::make_vector(std::move(v));
    }
    case ExprKind::COLUMN: {
      if (!ctx.schema) fail(ER_BAD_FIELD, "no table context");
      int idx = ctx.schema->find_column(e.s);
      if (idx < 0) fail(ER_BAD_FIELD, "Unknown column '" + e.s + "'");
      if (!ctx.row) fail(ER_BAD_FIELD, "column reference is not allowed here");
      return ctx.row->values[idx];
    }
    case ExprKind::L2_DISTANCE: {
      if (!ctx.schema || !ctx.row) fail(ER_BAD_FIELD, "no table context");
      int ci = ctx.schema->find_column(e.args[0]->s);
      if (ci < 0) fail(ER_BAD_FIELD, "Unknown column '" + e.args[0]->s + "'");
      const Value &v = ctx.row->values[ci];
      if (v.type != ValueType::VECTOR) fail(ER_INVALID_VECTOR, "not a vector");
      if (e.fnums.size() != v.v.size()) fail(ER_INVALID_VECTOR, "vector dimension mismatch");
      return Value::make_double(vector_l2(v.v, e.fnums));
    }
    case ExprKind::NEG: {
      Value v = eval_expr(*e.args[0], ctx);
      if (v.type == ValueType::INT) {
        if (v.i == std::numeric_limits<i64>::min()) fail(ER_OUT_OF_RANGE, "integer overflow");
        v.i = -v.i;
      } else if (v.type == ValueType::DOUBLE) {
        v.d = -v.d;
      } else {
        fail(ER_TRUNCATED_WRONG_VALUE, "cannot negate this value");
      }
      return v;
    }
    case ExprKind::ADD:
    case ExprKind::SUB:
    case ExprKind::MUL:
    case ExprKind::DIV: {
      Value a = eval_expr(*e.args[0], ctx);
      Value b = eval_expr(*e.args[1], ctx);
      bool ai = a.type == ValueType::INT;
      bool bi = b.type == ValueType::INT;
      if ((!ai && a.type != ValueType::DOUBLE) || (!bi && b.type != ValueType::DOUBLE))
        fail(ER_TRUNCATED_WRONG_VALUE, "arithmetic requires numeric operands");
      if (e.kind == ExprKind::DIV) {
        double x = ai ? static_cast<double>(a.i) : a.d;
        double y = bi ? static_cast<double>(b.i) : b.d;
        if (y == 0) fail(ER_TRUNCATED_WRONG_VALUE, "division by zero");
        return Value::make_double(x / y);
      }
      if (ai && bi) {
        __int128 r = 0;
        if (e.kind == ExprKind::ADD) r = static_cast<__int128>(a.i) + b.i;
        else if (e.kind == ExprKind::SUB) r = static_cast<__int128>(a.i) - b.i;
        else r = static_cast<__int128>(a.i) * b.i;
        if (r < std::numeric_limits<i64>::min() || r > std::numeric_limits<i64>::max())
          fail(ER_OUT_OF_RANGE, "integer overflow");
        return Value::make_int(static_cast<i64>(r));
      }
      double x = ai ? static_cast<double>(a.i) : a.d;
      double y = bi ? static_cast<double>(b.i) : b.d;
      double r = 0;
      if (e.kind == ExprKind::ADD) r = x + y;
      else if (e.kind == ExprKind::SUB) r = x - y;
      else r = x * y;
      if (!std::isfinite(r)) fail(ER_OUT_OF_RANGE, "numeric overflow");
      return Value::make_double(r);
    }
    case ExprKind::FUNC: {
      std::vector<Value> args;
      for (const auto &a : e.args) args.push_back(eval_expr(*a, ctx));
      if (e.s == "abs" && args.size() == 1) {
        if (args[0].type == ValueType::INT) return Value::make_int(std::llabs(args[0].i));
        if (args[0].type == ValueType::DOUBLE) return Value::make_double(std::fabs(args[0].d));
        fail(ER_TRUNCATED_WRONG_VALUE, "ABS requires a numeric argument");
      }
      if (e.s == "length" && args.size() == 1) {
        if (args[0].type == ValueType::STRING) return Value::make_int(args[0].s.size());
        fail(ER_TRUNCATED_WRONG_VALUE, "LENGTH requires a string argument");
      }
      if (e.s == "char_length" && args.size() == 1) {
        if (args[0].type == ValueType::STRING)
          return Value::make_int(static_cast<i64>(utf8_codepoints(args[0].s)));
        fail(ER_TRUNCATED_WRONG_VALUE, "CHAR_LENGTH requires a string argument");
      }
      if (e.s == "sqrt" && args.size() == 1) {
        double x = args[0].type == ValueType::INT ? static_cast<double>(args[0].i) : args[0].d;
        if (args[0].type != ValueType::INT && args[0].type != ValueType::DOUBLE)
          fail(ER_TRUNCATED_WRONG_VALUE, "SQRT requires a numeric argument");
        if (x < 0) fail(ER_TRUNCATED_WRONG_VALUE, "SQRT of negative number");
        return Value::make_double(std::sqrt(x));
      }
      fail(ER_UNSUPPORTED, "unsupported function '" + e.s + "'");
    }
  }
  fail(ER_UNSUPPORTED, "unsupported expression");
}

static int compare_values(const Value &a, const Value &b) {
  if ((a.type == ValueType::INT || a.type == ValueType::DOUBLE) &&
      (b.type == ValueType::INT || b.type == ValueType::DOUBLE)) {
    double x = a.type == ValueType::INT ? static_cast<double>(a.i) : a.d;
    double y = b.type == ValueType::INT ? static_cast<double>(b.i) : b.d;
    if (x < y) return -1;
    if (x > y) return 1;
    return 0;
  }
  if (a.type == ValueType::STRING && b.type == ValueType::STRING)
    return utf8_compare(a.s, b.s);
  if (a.type == ValueType::VECTOR && b.type == ValueType::VECTOR) {
    if (value_less(a, b)) return -1;
    if (value_less(b, a)) return 1;
    return 0;
  }
  fail(ER_TRUNCATED_WRONG_VALUE, "cannot compare these values");
}

bool eval_cond(const Cond &c, const EvalContext &ctx) {
  switch (c.kind) {
    case CondKind::AND:
      for (const auto &k : c.children)
        if (!eval_cond(*k, ctx)) return false;
      return true;
    case CondKind::OR:
      for (const auto &k : c.children)
        if (eval_cond(*k, ctx)) return true;
      return false;
    case CondKind::NOT: return !eval_cond(*c.children[0], ctx);
    case CondKind::COMPARE: {
      Value a = eval_expr(*c.left, ctx);
      Value b = eval_expr(*c.right, ctx);
      auto string_to_vector = [](const std::string &s) {
        bool ok = false;
        std::vector<double> nums = parse_vector_text(s, ok);
        if (!ok) fail(ER_INVALID_VECTOR, "invalid vector literal");
        std::vector<float> v(nums.size());
        for (size_t i = 0; i < nums.size(); ++i) {
          if (!std::isfinite(nums[i]) || std::fabs(nums[i]) > static_cast<double>(FLT_MAX))
            fail(ER_INVALID_VECTOR, "vector value out of float32 range");
          v[i] = static_cast<float>(nums[i]);
        }
        return Value::make_vector(std::move(v));
      };
      if (a.type == ValueType::VECTOR && b.type == ValueType::STRING)
        b = string_to_vector(b.s);
      else if (b.type == ValueType::VECTOR && a.type == ValueType::STRING)
        a = string_to_vector(a.s);
      int cmp = compare_values(a, b);
      switch (c.op) {
        case CmpOp::EQ: return cmp == 0;
        case CmpOp::NE: return cmp != 0;
        case CmpOp::LT: return cmp < 0;
        case CmpOp::LE: return cmp <= 0;
        case CmpOp::GT: return cmp > 0;
        case CmpOp::GE: return cmp >= 0;
      }
    }
  }
  return false;
}

bool eval_row_matches(const Cond *where, const TableSchema &schema, const Row &row,
                      const std::unordered_map<std::string, Value> *aliases) {
  if (!where) return true;
  EvalContext ctx;
  ctx.schema = &schema;
  ctx.row = &row;
  ctx.aliases = aliases;
  return eval_cond(*where, ctx);
}

// ===========================================================================
// Type coercion and literal evaluation.
// ===========================================================================
Value coerce_for_column(const Value &v, const Column &col) {
  switch (col.type) {
    case ColType::BIGINT: {
      if (v.type == ValueType::INT) return v;
      if (v.type == ValueType::DOUBLE) {
        if (!std::isfinite(v.d) || std::floor(v.d) != v.d ||
            v.d < static_cast<double>(std::numeric_limits<i64>::min()) ||
            v.d > static_cast<double>(std::numeric_limits<i64>::max()))
          fail(ER_OUT_OF_RANGE, "BIGINT value out of range");
        return Value::make_int(static_cast<i64>(v.d));
      }
      fail(ER_TRUNCATED_WRONG_VALUE, "cannot convert value to BIGINT");
    }
    case ColType::VARCHAR: {
      if (v.type != ValueType::STRING)
        fail(ER_TRUNCATED_WRONG_VALUE, "VARCHAR column requires a string value");
      if (!utf8_valid(v.s)) fail(ER_TRUNCATED_WRONG_VALUE, "invalid UTF-8 string");
      if (utf8_codepoints(v.s) > col.param)
        fail(ER_TOO_LONG, "Data too long for column '" + col.name + "'");
      return v;
    }
    case ColType::VECTOR: {
      std::vector<double> nums;
      if (v.type == ValueType::VECTOR) {
        nums.reserve(v.v.size());
        for (float f : v.v) nums.push_back(static_cast<double>(f));
      } else if (v.type == ValueType::STRING) {
        bool ok = false;
        nums = parse_vector_text(v.s, ok);
        if (!ok) fail(ER_INVALID_VECTOR, "invalid vector literal");
      } else {
        fail(ER_INVALID_VECTOR, "VECTOR column requires a vector value");
      }
      if (nums.size() != col.param)
        fail(ER_INVALID_VECTOR, "vector dimension mismatch for column '" + col.name + "'");
      std::vector<float> out(col.param);
      for (size_t i = 0; i < nums.size(); ++i) {
        double d = nums[i];
        if (!std::isfinite(d) || std::fabs(d) > static_cast<double>(FLT_MAX) ||
            (d != 0 && std::fabs(d) < static_cast<double>(FLT_TRUE_MIN) &&
             false))
          fail(ER_INVALID_VECTOR, "vector value out of float32 range");
        float f = static_cast<float>(d);
        if (!std::isfinite(f)) fail(ER_INVALID_VECTOR, "vector value out of float32 range");
        out[i] = f;
      }
      return Value::make_vector(std::move(out));
    }
  }
  fail(ER_UNSUPPORTED, "unsupported column type");
}

Value literal_value(const Expr &e) {
  EvalContext ctx;
  switch (e.kind) {
    case ExprKind::INT_LITERAL:
    case ExprKind::DOUBLE_LITERAL:
    case ExprKind::STRING_LITERAL:
      return eval_expr(e, ctx);
    case ExprKind::VECTOR_LITERAL: {
      std::vector<float> v(e.nums.size());
      for (size_t i = 0; i < e.nums.size(); ++i) {
        double d = e.nums[i];
        if (!std::isfinite(d) || std::fabs(d) > static_cast<double>(FLT_MAX))
          fail(ER_INVALID_VECTOR, "vector value out of float32 range");
        v[i] = static_cast<float>(d);
      }
      return Value::make_vector(std::move(v));
    }
    case ExprKind::NEG:
    case ExprKind::ADD:
    case ExprKind::SUB:
    case ExprKind::MUL:
    case ExprKind::DIV:
      return eval_expr(e, ctx);
    default:
      fail(ER_BAD_FIELD, "constant value expected");
  }
}

// ===========================================================================
// SELECT.
// ===========================================================================
std::string result_column_name(const SelectItem &item, const TableSchema &schema) {
  if (item.star) return "*";
  if (!item.alias.empty()) return item.alias;
  if (item.expr->kind == ExprKind::COLUMN) return item.expr->s;
  if (!item.expr->text.empty()) return item.expr->text;
  return "expr";
}

namespace {

struct Candidate {
  i64 id = 0;
  std::vector<Value> out;
  std::vector<Value> keys;
};

struct CandidateBetter {
  const std::vector<OrderItem> *order = nullptr;
  bool operator()(const Candidate &a, const Candidate &b) const {
    if (order && !order->empty()) {
      for (size_t i = 0; i < order->size(); ++i) {
        int c = compare_values(a.keys[i], b.keys[i]);
        if (c != 0) return (*order)[i].desc ? (c > 0) : (c < 0);
      }
    }
    return a.id < b.id;
  }
};

struct CandidateWorse {
  CandidateBetter better;
  bool operator()(const Candidate &a, const Candidate &b) const {
    // priority_queue keeps the "largest" element on top.  For a min-heap on
    // the desired order, the comparator must be the desired order.
    return better(a, b);
  }
};

}  // namespace

ResultSet Database::exec_select(const Statement &st) {
  const SelectStmt &s = st.select;
  Table *t = find_table(s.table);
  if (!t) fail(ER_UNKNOWN_TABLE, "Table '" + s.table + "' doesn't exist");
  const TableSchema &schema = t->schema();

  std::vector<SelectItem> items;
  if (s.items.size() == 1 && s.items[0].star) {
    for (const auto &c : schema.columns) {
      SelectItem it;
      it.expr = make_expr(ExprKind::COLUMN);
      it.expr->s = c.name;
      it.expr->text = c.name;
      items.push_back(std::move(it));
    }
  } else {
    for (const auto &it : s.items) {
      if (it.star) {
        for (const auto &c : schema.columns) {
          SelectItem x;
          x.expr = make_expr(ExprKind::COLUMN);
          x.expr->s = c.name;
          x.expr->text = c.name;
          items.push_back(std::move(x));
        }
      } else {
        SelectItem x;
        x.alias = it.alias;
        x.expr = it.expr ? clone_expr(*it.expr) : nullptr;
        items.push_back(std::move(x));
      }
    }
  }

  for (auto &it : items) {
    if (!it.expr) continue;
    prepare_expr(*it.expr, schema);
    validate_expr_columns(*it.expr, schema);
  }
  if (s.where) {
    validate_cond_columns(*s.where, schema);
    // Prepare L2_DISTANCE in WHERE (unusual but supported).
    std::function<void(Cond &)> prep = [&](Cond &c) {
      if (c.kind == CondKind::COMPARE) {
        prepare_expr(*c.left, schema);
        prepare_expr(*c.right, schema);
      } else {
        for (auto &k : c.children) prep(*k);
      }
    };
    prep(*s.where);
  }
  for (auto &o : s.order) {
    bool alias_ref = false;
    if (o.expr->kind == ExprKind::COLUMN) {
      for (const auto &it : items) {
        if (!it.alias.empty() && iequals(it.alias, o.expr->s)) {
          alias_ref = true;
          break;
        }
      }
    }
    if (!alias_ref) {
      prepare_expr(*o.expr, schema);
      validate_expr_columns(*o.expr, schema);
    }
  }

  ResultSet rs;
  rs.query = true;
  for (const auto &it : items) rs.columns.push_back(result_column_name(it, schema));

  // Fast path for the dominant benchmark query shape:
  //   SELECT ... FROM t [WHERE ...] ORDER BY L2_DISTANCE(v, Q) [alias] LIMIT k
  // It avoids building a Candidate (and a small vector) for every row.
  {
    const Expr *dist = nullptr;
    if (s.order.size() == 1 && !s.order[0].desc) {
      if (s.order[0].expr->kind == ExprKind::L2_DISTANCE) {
        dist = s.order[0].expr.get();
      } else if (s.order[0].expr->kind == ExprKind::COLUMN) {
        for (const auto &it : items) {
          if (!it.alias.empty() && iequals(it.alias, s.order[0].expr->s) && it.expr &&
              it.expr->kind == ExprKind::L2_DISTANCE) {
            dist = it.expr.get();
            break;
          }
        }
      }
    }
    int vc = schema.vector_index();
    bool no_overlay = overlay_.find(t) == overlay_.end();
    if (dist && vc >= 0 && no_overlay &&
        dist->fnums.size() == schema.columns[vc].param) {
      const auto *snap = t->scan_snapshot();
      if (snap) {
        struct FastCand {
          double d;
          i64 id;
          const Row *row;
        };
        struct FastWorse {
          bool operator()(const FastCand &a, const FastCand &b) const {
            if (a.d != b.d) return a.d < b.d;
            return a.id < b.id;
          }
        };
        size_t cap = 0;
        bool bounded = false;
        if (s.limit >= 0) {
          u64 c = static_cast<u64>(s.limit) + static_cast<u64>(s.offset);
          if (c < 10000000ull) {
            cap = static_cast<size_t>(c);
            bounded = true;
          }
        }
        std::priority_queue<FastCand, std::vector<FastCand>, FastWorse> heap;
        std::vector<FastCand> all;
        if (!bounded) all.reserve(snap->size());
        size_t dim = schema.columns[vc].param;
        for (const auto &e : *snap) {
          if (!e.vec) continue;
          if (s.where) {
            EvalContext ctx;
            ctx.schema = &schema;
            ctx.row = e.row;
            if (!eval_cond(*s.where, ctx)) continue;
          }
          double d = vector_l2_ptr(e.vec, dist->fnums.data(), dim);
          FastCand fc{d, e.id, e.row};
          if (bounded) {
            if (cap == 0) continue;
            if (heap.size() < cap) {
              heap.push(fc);
            } else {
              const FastCand &w = heap.top();
              if (d < w.d || (d == w.d && e.id < w.id)) {
                heap.pop();
                heap.push(fc);
              }
            }
          } else {
            all.push_back(fc);
          }
        }
        std::vector<FastCand> result;
        if (bounded) {
          result.reserve(heap.size());
          while (!heap.empty()) {
            result.push_back(heap.top());
            heap.pop();
          }
        } else {
          result = std::move(all);
        }
        std::sort(result.begin(), result.end(), [](const FastCand &a, const FastCand &b) {
          if (a.d != b.d) return a.d < b.d;
          return a.id < b.id;
        });
        size_t start = static_cast<size_t>(
            std::min<i64>(s.offset, static_cast<i64>(result.size())));
        size_t end = result.size();
        if (s.limit >= 0) end = std::min(end, start + static_cast<size_t>(s.limit));
        rs.rows.reserve(end - start);
        for (size_t i = start; i < end; ++i) {
          EvalContext ctx;
          ctx.schema = &schema;
          ctx.row = result[i].row;
          std::vector<std::string> out;
          out.reserve(items.size());
          for (const auto &it : items) out.push_back(value_to_string(eval_expr(*it.expr, ctx)));
          rs.rows.push_back(std::move(out));
        }
        return rs;
      }
    }
  }


  // Collect visible ids.  Base rows first, then overlay-only rows.
  CandidateBetter better;
  better.order = &s.order;
  size_t cap = 0;
  bool use_heap = false;
  if (s.limit >= 0) {
    u64 c = static_cast<u64>(s.limit) + static_cast<u64>(s.offset);
    if (c < 10000000ull) {
      cap = static_cast<size_t>(c);
      use_heap = true;
    }
  }
  std::priority_queue<Candidate, std::vector<Candidate>, CandidateWorse> heap(
      CandidateWorse{better});
  std::vector<Candidate> all;
  if (!use_heap) all.reserve(t->rows().size());

  size_t matched = 0;
  std::vector<Value> keys_buf;
  auto better_than_top = [&](const std::vector<Value> &keys, i64 id, const Candidate &top) {
    for (size_t i = 0; i < s.order.size(); ++i) {
      int c = compare_values(keys[i], top.keys[i]);
      if (c != 0) return s.order[i].desc ? (c > 0) : (c < 0);
    }
    return id < top.id;
  };
  auto handle = [&](i64 id, const Row *rowp) {
    if (!rowp) return;
    const Row &row = *rowp;
    EvalContext ctx;
    ctx.schema = &schema;
    ctx.row = &row;
    if (s.where && !eval_cond(*s.where, ctx)) return;
    ++matched;

    keys_buf.clear();
    keys_buf.reserve(s.order.size());
    for (const auto &o : s.order) {
      bool done = false;
      if (o.expr->kind == ExprKind::COLUMN) {
        for (size_t k = 0; k < items.size(); ++k) {
          if (!items[k].alias.empty() && iequals(items[k].alias, o.expr->s)) {
            keys_buf.push_back(eval_expr(*items[k].expr, ctx));
            done = true;
            break;
          }
        }
      }
      if (!done) keys_buf.push_back(eval_expr(*o.expr, ctx));
    }

    if (use_heap && heap.size() == cap && cap > 0) {
      if (!better_than_top(keys_buf, id, heap.top())) return;
    }

    Candidate cand;
    cand.id = id;
    cand.out.reserve(items.size());
    for (const auto &it : items) cand.out.push_back(eval_expr(*it.expr, ctx));
    cand.keys = keys_buf;

    if (use_heap) {
      if (cap == 0) return;
      if (heap.size() < cap) {
        heap.push(std::move(cand));
      } else {
        heap.pop();
        heap.push(std::move(cand));
      }
    } else {
      all.push_back(std::move(cand));
    }
  };

  auto ov = overlay_.find(t);
  const TxnOverlay *ovl = ov == overlay_.end() ? nullptr : &ov->second;
  if (!ovl) {
    if (const auto *snap = t->scan_snapshot()) {
      for (const auto &e : *snap) handle(e.id, e.row);
    } else {
      for (const auto &kv : t->rows()) {
        const Row *rp = t->cached(kv.first);
        std::optional<Row> tmp;
        if (!rp) {
          tmp = t->load_row_by_id(kv.first);
          if (!tmp) continue;
          rp = &*tmp;
        }
        handle(kv.first, rp);
      }
    }
  } else {
    std::vector<i64> ids;
    ids.reserve(t->rows().size() + ovl->rows.size());
    for (const auto &kv : t->rows()) {
      if (ovl->rows.count(kv.first)) continue;
      ids.push_back(kv.first);
    }
    for (const auto &kv : ovl->rows) {
      if (kv.second.has_value()) ids.push_back(kv.first);
    }
    for (i64 id : ids) {
      const Row *rp = nullptr;
      auto r = ovl->rows.find(id);
      if (r != ovl->rows.end()) {
        if (!r->second.has_value()) continue;
        rp = &*r->second;
      }
      std::optional<Row> tmp;
      if (!rp) {
        rp = t->cached(id);
        if (!rp) {
          tmp = t->load_row_by_id(id);
          if (!tmp) continue;
          rp = &*tmp;
        }
      }
      handle(id, rp);
    }
  }

  std::vector<Candidate> result;
  if (use_heap) {
    result.reserve(heap.size());
    while (!heap.empty()) {
      result.push_back(std::move(const_cast<Candidate &>(heap.top())));
      heap.pop();
    }
    std::sort(result.begin(), result.end(), better);
  } else {
    std::sort(all.begin(), all.end(), better);
    result = std::move(all);
  }

  size_t start = static_cast<size_t>(std::min<i64>(s.offset, static_cast<i64>(result.size())));
  size_t end = result.size();
  if (s.limit >= 0) end = std::min(end, start + static_cast<size_t>(s.limit));
  rs.rows.reserve(end - start);
  for (size_t i = start; i < end; ++i) {
    std::vector<std::string> out;
    out.reserve(result[i].out.size());
    for (const auto &v : result[i].out) out.push_back(value_to_string(v));
    rs.rows.push_back(std::move(out));
  }
  (void)matched;
  return rs;
}

// ===========================================================================
// INSERT.
// ===========================================================================
ResultSet Database::exec_insert(const Statement &st) {
  const InsertStmt &s = st.insert;
  Table *t = find_table(s.table);
  if (!t) fail(ER_UNKNOWN_TABLE, "Table '" + s.table + "' doesn't exist");
  const TableSchema &schema = t->schema();
  size_t ncols = schema.columns.size();

  std::vector<int> mapping;
  if (s.columns.empty()) {
    for (size_t i = 0; i < ncols; ++i) mapping.push_back(static_cast<int>(i));
  } else {
    std::set<int> seen;
    for (const auto &c : s.columns) {
      int idx = schema.find_column(c);
      if (idx < 0) fail(ER_BAD_FIELD, "Unknown column '" + c + "' in 'field list'");
      if (!seen.insert(idx).second) fail(ER_BAD_FIELD, "Column '" + c + "' specified twice");
      mapping.push_back(idx);
    }
    if (mapping.size() != ncols) {
      for (size_t i = 0; i < ncols; ++i) {
        if (!seen.count(static_cast<int>(i)))
          fail(ER_BAD_NULL, "Column '" + schema.columns[i].name + "' cannot be null");
      }
    }
  }

  std::vector<Change> changes;
  changes.reserve(s.rows.size());
  std::unordered_set<i64> seen_pk;
  int pk = schema.pk_index;
  if (pk < 0) fail(1105, "table has no primary key");

  for (const auto &exprs : s.rows) {
    if (exprs.size() != mapping.size())
      fail(ER_WRONG_VALUE_COUNT, "Column count doesn't match value count");
    Row row;
    row.values.resize(ncols);
    std::vector<bool> assigned(ncols, false);
    for (size_t i = 0; i < exprs.size(); ++i) {
      if (!exprs[i]) fail(ER_PARSE, "missing value");
      Value v = literal_value(*exprs[i]);
      row.values[mapping[i]] = coerce_for_column(v, schema.columns[mapping[i]]);
      assigned[mapping[i]] = true;
    }
    for (size_t i = 0; i < ncols; ++i) {
      if (!assigned[i])
        fail(ER_BAD_NULL, "Column '" + schema.columns[i].name + "' cannot be null");
    }
    i64 id = row.values[pk].i;
    if (!seen_pk.insert(id).second)
      fail(ER_DUP_ENTRY, "Duplicate entry '" + std::to_string(id) + "' for key 'PRIMARY'");
    if (visible_row(t, id))
      fail(ER_DUP_ENTRY, "Duplicate entry '" + std::to_string(id) + "' for key 'PRIMARY'");
    Change c;
    c.table = t;
    c.id = id;
    c.deleted = false;
    c.row = std::move(row);
    changes.push_back(std::move(c));
  }

  if (txn_active_) {
    for (auto &c : changes) set_overlay(t, c.id, std::move(c.row));
  } else {
    commit_changes(changes);
  }
  ResultSet rs;
  rs.affected = static_cast<i64>(changes.size());
  return rs;
}

// ===========================================================================
// UPDATE.
// ===========================================================================
ResultSet Database::exec_update(const Statement &st) {
  const UpdateStmt &s = st.update;
  Table *t = find_table(s.table);
  if (!t) fail(ER_UNKNOWN_TABLE, "Table '" + s.table + "' doesn't exist");
  const TableSchema &schema = t->schema();
  int pk = schema.pk_index;

  std::vector<std::pair<int, ExprPtr>> sets;
  for (const auto &kv : s.sets) {
    int idx = schema.find_column(kv.first);
    if (idx < 0) fail(ER_BAD_FIELD, "Unknown column '" + kv.first + "' in 'field list'");
    if (idx == pk) fail(ER_UNSUPPORTED, "primary key updates are not supported");
    ExprPtr e = clone_expr(*kv.second);
    prepare_expr(*e, schema);
    sets.emplace_back(idx, std::move(e));
  }
  if (s.where) {
    validate_cond_columns(*s.where, schema);
    std::function<void(Cond &)> prep = [&](Cond &c) {
      if (c.kind == CondKind::COMPARE) {
        prepare_expr(*c.left, schema);
        prepare_expr(*c.right, schema);
      } else {
        for (auto &k : c.children) prep(*k);
      }
    };
    prep(*s.where);
  }

  std::vector<i64> ids;
  auto ov = overlay_.find(t);
  const TxnOverlay *ovl = ov == overlay_.end() ? nullptr : &ov->second;
  for (const auto &kv : t->rows()) {
    if (ovl && ovl->rows.count(kv.first)) continue;
    ids.push_back(kv.first);
  }
  if (ovl) {
    for (const auto &kv : ovl->rows)
      if (kv.second.has_value()) ids.push_back(kv.first);
  }

  std::vector<Change> changes;
  for (i64 id : ids) {
    const Row *rowp = nullptr;
    std::optional<Row> tmp;
    if (ovl) {
      auto r = ovl->rows.find(id);
      if (r != ovl->rows.end()) {
        if (!r->second.has_value()) continue;
        rowp = &*r->second;
      }
    }
    if (!rowp) {
      rowp = t->cached(id);
      if (!rowp) {
        tmp = t->load_row_by_id(id);
        if (!tmp) continue;
        rowp = &*tmp;
      }
    }
    const Row &row = *rowp;

    EvalContext ctx;
    ctx.schema = &schema;
    ctx.row = &row;
    if (s.where && !eval_cond(*s.where, ctx)) continue;

    Row nr = row;
    for (auto &set : sets) {
      Value v = eval_expr(*set.second, ctx);
      nr.values[set.first] = coerce_for_column(v, schema.columns[set.first]);
    }
    Change c;
    c.table = t;
    c.id = id;
    c.deleted = false;
    c.row = std::move(nr);
    changes.push_back(std::move(c));
  }

  if (txn_active_) {
    for (auto &c : changes) set_overlay(t, c.id, std::move(c.row));
  } else {
    commit_changes(changes);
  }
  ResultSet rs;
  rs.affected = static_cast<i64>(changes.size());
  return rs;
}

// ===========================================================================
// DELETE.
// ===========================================================================
ResultSet Database::exec_delete(const Statement &st) {
  const DeleteStmt &s = st.del;
  Table *t = find_table(s.table);
  if (!t) fail(ER_UNKNOWN_TABLE, "Table '" + s.table + "' doesn't exist");
  const TableSchema &schema = t->schema();
  if (s.where) {
    validate_cond_columns(*s.where, schema);
    std::function<void(Cond &)> prep = [&](Cond &c) {
      if (c.kind == CondKind::COMPARE) {
        prepare_expr(*c.left, schema);
        prepare_expr(*c.right, schema);
      } else {
        for (auto &k : c.children) prep(*k);
      }
    };
    prep(*s.where);
  }

  std::vector<i64> ids;
  auto ov = overlay_.find(t);
  const TxnOverlay *ovl = ov == overlay_.end() ? nullptr : &ov->second;
  for (const auto &kv : t->rows()) {
    if (ovl && ovl->rows.count(kv.first)) continue;
    ids.push_back(kv.first);
  }
  if (ovl) {
    for (const auto &kv : ovl->rows)
      if (kv.second.has_value()) ids.push_back(kv.first);
  }

  std::vector<Change> changes;
  for (i64 id : ids) {
    const Row *rowp = nullptr;
    std::optional<Row> tmp;
    if (ovl) {
      auto r = ovl->rows.find(id);
      if (r != ovl->rows.end()) {
        if (!r->second.has_value()) continue;
        rowp = &*r->second;
      }
    }
    if (!rowp) {
      rowp = t->cached(id);
      if (!rowp) {
        tmp = t->load_row_by_id(id);
        if (!tmp) continue;
        rowp = &*tmp;
      }
    }
    EvalContext ctx;
    ctx.schema = &schema;
    ctx.row = rowp;
    if (s.where && !eval_cond(*s.where, ctx)) continue;
    Change c;
    c.table = t;
    c.id = id;
    c.deleted = true;
    changes.push_back(std::move(c));
  }

  if (txn_active_) {
    for (auto &c : changes) set_overlay(t, c.id, std::nullopt);
  } else {
    commit_changes(changes);
  }
  ResultSet rs;
  rs.affected = static_cast<i64>(changes.size());
  return rs;
}

// ===========================================================================
// DDL.
// ===========================================================================
void Database::ensure_no_txn_for_ddl() {
  // MySQL semantics: DDL causes an implicit commit of the active transaction.
  if (txn_active_) commit_txn();
}

ResultSet Database::exec_create_table(const Statement &st) {
  ensure_no_txn_for_ddl();
  const CreateTableStmt &s = st.create_table;
  const std::string lname = to_lower(s.table);
  if (find_table(s.table)) {
    if (s.if_not_exists) return ResultSet{};
    fail(ER_TABLE_EXISTS, "Table '" + s.table + "' already exists");
  }
  if (s.columns.empty()) fail(ER_PARSE, "table must have at least one column");

  TableSchema schema;
  schema.name = s.table;
  schema.id = static_cast<u32>(next_table_id_++);
  int pk = -1;
  int vector_cols = 0;
  std::set<std::string> names;
  for (const auto &cd : s.columns) {
    if (!names.insert(to_lower(cd.name)).second)
      fail(ER_PARSE, "duplicate column name '" + cd.name + "'");
    Column c;
    c.name = cd.name;
    c.type = cd.type;
    c.param = cd.param;
    if (cd.type == ColType::VECTOR) ++vector_cols;
    if (cd.primary) {
      if (pk >= 0) fail(ER_PARSE, "only one primary key is allowed");
      pk = static_cast<int>(schema.columns.size());
      if (cd.type != ColType::BIGINT) fail(ER_PARSE, "primary key must be BIGINT");
    }
    schema.columns.push_back(std::move(c));
  }
  if (!s.primary_key.empty()) {
    if (s.primary_key.size() != 1) fail(ER_PARSE, "only one primary key is allowed");
    if (pk >= 0) fail(ER_PARSE, "primary key specified twice");
    pk = schema.find_column(s.primary_key[0]);
    if (pk < 0) fail(ER_BAD_FIELD, "Unknown column '" + s.primary_key[0] + "'");
    if (schema.columns[pk].type != ColType::BIGINT)
      fail(ER_PARSE, "primary key must be BIGINT");
  }
  if (pk < 0) fail(ER_PARSE, "a BIGINT primary key is required");
  if (vector_cols > 1) fail(ER_PARSE, "at most one VECTOR column is allowed");
  schema.pk_index = pk;

  std::string path = dir_ + "/tbl_" + std::to_string(schema.id) + ".dat";
  auto table = std::make_unique<Table>(this, schema, path);
  table->open_data();
  tables_[schema.id] = std::move(table);
  table_by_name_[lname] = schema.id;
  try {
    save_catalog();
  } catch (...) {
    tables_.erase(schema.id);
    table_by_name_.erase(lname);
    ::unlink(path.c_str());
    throw;
  }
  return ResultSet{};
}

ResultSet Database::exec_create_index(const Statement &st) {
  ensure_no_txn_for_ddl();
  const CreateIndexStmt &s = st.create_index;
  Table *t = find_table(s.table);
  if (!t) fail(ER_UNKNOWN_TABLE, "Table '" + s.table + "' doesn't exist");
  TableSchema &schema = t->schema();
  int ci = schema.find_column(s.column);
  if (ci < 0) fail(ER_BAD_FIELD, "Unknown column '" + s.column + "'");
  if (schema.columns[ci].type != ColType::VECTOR)
    fail(ER_UNSUPPORTED, "vector index must be created on a VECTOR column");
  for (const auto &ix : schema.vector_indexes) {
    if (iequals(ix.first, s.index)) {
      if (s.if_not_exists) return ResultSet{};
      fail(1061, "Duplicate key name '" + s.index + "'");
    }
  }
  schema.vector_indexes.emplace_back(s.index, schema.columns[ci].name);
  save_catalog();
  return ResultSet{};
}

ResultSet Database::exec_drop_table(const Statement &st) {
  ensure_no_txn_for_ddl();
  const DropTableStmt &s = st.drop_table;
  Table *t = find_table(s.table);
  if (!t) {
    if (s.if_exists) return ResultSet{};
    fail(ER_UNKNOWN_TABLE, "Table '" + s.table + "' doesn't exist");
  }
  u32 id = t->schema().id;
  std::string lname = to_lower(t->schema().name);
  std::string path = dir_ + "/tbl_" + std::to_string(id) + ".dat";
  tables_.erase(id);
  table_by_name_.erase(lname);
  save_catalog();
  ::unlink(path.c_str());
  fsync_dir(dir_);
  return ResultSet{};
}

ResultSet Database::exec_drop_index(const Statement &st) {
  ensure_no_txn_for_ddl();
  const DropIndexStmt &s = st.drop_index;
  Table *t = find_table(s.table);
  if (!t) fail(ER_UNKNOWN_TABLE, "Table '" + s.table + "' doesn't exist");
  auto &v = t->schema().vector_indexes;
  auto it = std::find_if(v.begin(), v.end(),
                         [&](const std::pair<std::string, std::string> &p) {
                           return iequals(p.first, s.index);
                         });
  if (it == v.end()) {
    if (s.if_exists) return ResultSet{};
    fail(1091, "Can't DROP '" + s.index + "'; check that column/key exists");
  }
  v.erase(it);
  save_catalog();
  return ResultSet{};
}

ResultSet Database::exec_set(const Statement &st) {
  std::string name = to_lower(st.set.name);
  Value v;
  try {
    v = literal_value(*st.set.value);
  } catch (const Error &) {
    fail(ER_TRUNCATED_WRONG_VALUE, "invalid value for " + name);
  }
  if (name == "search_mode" || name == "vector_search_mode" || name == "ann" ||
      name == "vector_index_mode") {
    std::string sval;
    if (v.type == ValueType::STRING) sval = to_lower(v.s);
    else if (v.type == ValueType::INT) sval = v.i ? "ann" : "exact";
    else fail(ER_TRUNCATED_WRONG_VALUE, "invalid mode value");
    if (sval == "ann" || sval == "approximate" || sval == "1" || sval == "on")
      default_mode_ = "ann";
    else if (sval == "exact" || sval == "0" || sval == "off")
      default_mode_ = "exact";
    else
      fail(ER_TRUNCATED_WRONG_VALUE, "unknown search mode '" + sval + "'");
    return ResultSet{};
  }
  if (name == "autocommit") return ResultSet{};
  fail(ER_UNSUPPORTED, "unknown system variable '" + st.set.name + "'");
}

ResultSet Database::exec_show_tables() {
  ResultSet rs;
  rs.query = true;
  rs.columns = {"Tables"};
  std::vector<std::string> names;
  for (const auto &kv : tables_) names.push_back(kv.second->schema().name);
  std::sort(names.begin(), names.end());
  for (const auto &n : names) rs.rows.push_back({n});
  return rs;
}

ResultSet Database::exec_describe(const Statement &st) {
  Table *t = find_table(st.describe.table);
  if (!t) fail(ER_UNKNOWN_TABLE, "Table '" + st.describe.table + "' doesn't exist");
  ResultSet rs;
  rs.query = true;
  rs.columns = {"Field", "Type", "Key"};
  for (size_t i = 0; i < t->schema().columns.size(); ++i) {
    const auto &c = t->schema().columns[i];
    std::string type;
    if (c.type == ColType::BIGINT) type = "bigint";
    else if (c.type == ColType::VARCHAR) type = "varchar(" + std::to_string(c.param) + ")";
    else type = "vector(" + std::to_string(c.param) + ")";
    rs.rows.push_back({c.name, type, static_cast<int>(i) == t->schema().pk_index ? "PRI" : ""});
  }
  return rs;
}

// ===========================================================================
// Dispatch.
// ===========================================================================
ResultSet Database::execute(const Statement &st) {
  switch (st.kind) {
    case StmtKind::EMPTY: return ResultSet{};
    case StmtKind::SELECT: return exec_select(st);
    case StmtKind::INSERT: return exec_insert(st);
    case StmtKind::UPDATE: return exec_update(st);
    case StmtKind::DELETE: return exec_delete(st);
    case StmtKind::CREATE_TABLE: return exec_create_table(st);
    case StmtKind::CREATE_INDEX: return exec_create_index(st);
    case StmtKind::DROP_TABLE: return exec_drop_table(st);
    case StmtKind::DROP_INDEX: return exec_drop_index(st);
    case StmtKind::BEGIN:
      begin_txn();
      return ResultSet{};
    case StmtKind::COMMIT:
      commit_txn();
      return ResultSet{};
    case StmtKind::ROLLBACK:
      rollback_txn();
      return ResultSet{};
    case StmtKind::SET: return exec_set(st);
    case StmtKind::SHOW_TABLES: return exec_show_tables();
    case StmtKind::DESCRIBE: return exec_describe(st);
    case StmtKind::QUIT: return ResultSet{};
  }
  fail(ER_UNSUPPORTED, "unsupported statement");
}

}  // namespace mvdb
