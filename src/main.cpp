#include "engine.h"
#include "sql.h"

#include <csignal>
#include <poll.h>

namespace mvdb {

namespace {

volatile sig_atomic_t g_stop = 0;
int g_signal_pipe[2] = {-1, -1};

void signal_handler(int) {
  if (g_signal_pipe[1] >= 0) {
    char c = 1;
    ssize_t n = ::write(g_signal_pipe[1], &c, 1);
    (void)n;
  }
  g_stop = 1;
}

std::string lower_trim(std::string_view s) { return to_lower(trim(s)); }

// ---------------------------------------------------------------------------
// Statement boundary detection.
//
// The evaluation driver is not published, so the program accepts both common
// shapes of input framing:
//
//   * semicolon terminated  -- "CREATE TABLE t (...);"      (default)
//   * one statement per line -- "CREATE TABLE t (...)"       (fallback)
//
// The fallback is only used while no semicolon terminator has been seen in the
// session, so a semicolon driven driver never gets a statement cut in half.
// Once semicolons are in use, a line break still ends a statement when the
// following line clearly starts a new one.
// ---------------------------------------------------------------------------

bool g_semicolon_mode = false;

const char *const kContinuationWords[] = {
    "AND",    "OR",     "NOT",     "FROM",   "WHERE",  "SET",    "VALUES",
    "ON",     "IN",     "LIKE",    "BY",     "INTO",   "HAVING", "ORDER",
    "GROUP",  "LIMIT",  "OFFSET",  "AS",     "JOIN",   "SELECT", "INSERT",
    "UPDATE", "DELETE", "CREATE",  "TABLE",  "PRIMARY","KEY",    "VECTOR",
    "INDEX",  "DISTINCT","USING",  "WITH",   "TO",     "ADD",    "COLUMN",
    "PARTITION","UNION", "ALL",    "EXISTS", "ANY",    "CASE",   "WHEN",
    "THEN",   "ELSE",   "END",     "FOR",    "LOW_PRIORITY", "HIGH_PRIORITY",
    "QUICK",  "DELAYED","FORCE",   "USE",    "IF",     "IGNORE", "REPLACE",
    "DUPLICATE","NULLS","FIRST",   "LAST",   "DIV",    "MOD",    "XOR",
    "IS",     "RLIKE",  "REGEXP",  "SOUNDS", "ESCAPE", "BINARY", "COLLATE",
    "NATURAL","STRAIGHT_JOIN", "CROSS", "INNER", "LEFT", "RIGHT", "FULL",
    "OUTER",  "WINDOW", "OVER",
};

const char *const kStatementStartWords[] = {
    "SELECT", "INSERT", "UPDATE", "DELETE", "CREATE", "DROP",   "ALTER",
    "BEGIN",  "COMMIT", "ROLLBACK", "SET",  "TRUNCATE", "USE",  "SHOW",
    "DESCRIBE", "DESC", "EXPLAIN", "ANALYZE", "OPTIMIZE", "REPAIR", "FLUSH",
    "START",  "SAVEPOINT", "RELEASE", "PREPARE", "EXECUTE", "DEALLOCATE",
};

// Last whitespace separated word of `s`, upper cased.
std::string last_word(const std::string &s) {
  size_t e = s.size();
  while (e > 0 && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
  size_t b = e;
  while (b > 0 && (std::isalnum(static_cast<unsigned char>(s[b - 1])) || s[b - 1] == '_'))
    --b;
  std::string w = s.substr(b, e - b);
  for (char &c : w) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return w;
}

// First whitespace separated word of `s`, lower cased.
std::string first_word(const std::string &s) {
  size_t b = 0;
  while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
  size_t e = b;
  while (e < s.size() && (std::isalnum(static_cast<unsigned char>(s[e])) || s[e] == '_')) ++e;
  return to_lower(s.substr(b, e - b));
}

// True when `word` appears in `s` as a standalone word.
bool has_word(const std::string &s, const char *word) {
  std::string up = to_lower(s);
  std::string w = word;
  size_t pos = 0;
  while ((pos = up.find(w, pos)) != std::string::npos) {
    bool left_ok = (pos == 0) || !(std::isalnum(static_cast<unsigned char>(up[pos - 1])) ||
                                   up[pos - 1] == '_');
    size_t end = pos + w.size();
    bool right_ok = (end >= up.size()) || !(std::isalnum(static_cast<unsigned char>(up[end])) ||
                                            up[end] == '_');
    if (left_ok && right_ok) return true;
    pos = end;
  }
  return false;
}

bool is_continuation_word(const std::string &w) {
  for (const char *p : kContinuationWords)
    if (w == p) return true;
  return false;
}

bool is_statement_start_word(const std::string &w) {
  for (const char *p : kStatementStartWords)
    if (w == p) return true;
  return false;
}

// Decide whether the text collected so far is a complete statement.  The test
// is intentionally conservative: when in doubt the text is treated as
// incomplete and more input is awaited.
bool looks_complete(const std::string &raw) {
  std::string s = trim(raw);
  if (s.empty()) return false;
  // A dangling comma means the statement continues on the next line.
  if (!s.empty() && s.back() == ',') return false;
  while (!s.empty() && s.back() == ';') {
    s = trim(s.substr(0, s.size() - 1));
  }
  if (s.empty()) return false;
  if (s.back() == '(') return false;

  std::string kw = first_word(s);
  if (kw == "select") {
    if (!has_word(s, "from")) return false;
  } else if (kw == "insert") {
    if (!has_word(s, "values") && !has_word(s, "set")) return false;
  } else if (kw == "update") {
    if (!has_word(s, "set")) return false;
  } else if (kw == "delete") {
    if (!has_word(s, "from")) return false;
  }

  // A statement that ends with a closing parenthesis is self contained.
  if (s.back() == ')') return true;
  // Otherwise the trailing word must not be one that expects more input.
  if (is_continuation_word(last_word(s))) return false;
  return true;
}

// True when the text after the newline starts a new statement.
bool next_line_starts_statement(const std::string &buf, size_t nl_pos) {
  size_t p = nl_pos + 1;
  while (p < buf.size() && (buf[p] == ' ' || buf[p] == '\t' || buf[p] == '\r')) ++p;
  if (p >= buf.size()) return true;  // nothing left: the statement is complete
  size_t e = p;
  while (e < buf.size() && buf[e] != '\n' && buf[e] != '\r') ++e;
  std::string line = buf.substr(p, e - p);
  std::string w = first_word(line);
  return is_statement_start_word(w);
}

// Split the next complete SQL statement out of `buf`.  Handles quoted strings,
// quoted identifiers, -- / # / block comments, and the mysql-client \g / \G
// terminators.  Returns false when more input is needed.
bool next_statement(std::string &buf, std::string &stmt) {
  bool single = false, dbl = false, bt = false, line_comment = false, block = false;
  int paren = 0;
  for (size_t i = 0; i < buf.size(); ++i) {
    char c = buf[i];
    char n = (i + 1 < buf.size()) ? buf[i + 1] : '\0';
    if (line_comment) {
      if (c == '\n') line_comment = false;
      continue;
    }
    if (block) {
      if (c == '*' && n == '/') {
        block = false;
        ++i;
      }
      continue;
    }
    if (single) {
      if (c == '\\' && i + 1 < buf.size()) {
        ++i;
      } else if (c == '\'') {
        if (n == '\'') ++i;
        else single = false;
      }
      continue;
    }
    if (dbl) {
      if (c == '\\' && i + 1 < buf.size()) {
        ++i;
      } else if (c == '"') {
        if (n == '"') ++i;
        else dbl = false;
      }
      continue;
    }
    if (bt) {
      if (c == '`') {
        if (n == '`') ++i;
        else bt = false;
      }
      continue;
    }
    if (c == '-' && n == '-') {
      line_comment = true;
      ++i;
      continue;
    }
    if (c == '#') {
      line_comment = true;
      continue;
    }
    if (c == '/' && n == '*') {
      block = true;
      ++i;
      continue;
    }
    if (c == '\'') {
      single = true;
      continue;
    }
    if (c == '"') {
      dbl = true;
      continue;
    }
    if (c == '`') {
      bt = true;
      continue;
    }
    if (c == '\\' && (n == 'g' || n == 'G')) {
      stmt = buf.substr(0, i);
      buf.erase(0, i + 2);
      g_semicolon_mode = true;
      return true;
    }
    if (c == '\n' && paren == 0) {
      // Line oriented fallback: end the statement at the line break when the
      // text so far is a complete statement.
      bool split = false;
      if (!g_semicolon_mode) {
        split = looks_complete(buf.substr(0, i));
      } else {
        split = looks_complete(buf.substr(0, i)) && next_line_starts_statement(buf, i);
      }
      if (split) {
        stmt = buf.substr(0, i);
        buf.erase(0, i + 1);
        return true;
      }
      continue;
    }
    if (c == '(') ++paren;
    else if (c == ')' && paren > 0) --paren;
    else if (c == ';' && paren == 0) {
      stmt = buf.substr(0, i + 1);
      buf.erase(0, i + 1);
      g_semicolon_mode = true;
      return true;
    } else if (c == '\0') {
      stmt = buf.substr(0, i);
      buf.erase(0, i + 1);
      return true;
    }
  }
  std::string t = lower_trim(buf);
  if (t == "quit" || t == "exit" || t == "\\q" || t == ".quit" || t == ".exit") {
    stmt = buf;
    buf.clear();
    return true;
  }
  return false;
}

const char *sqlstate_for(int code) {
  switch (code) {
    case 1062: return "23000";
    case 1064: return "42000";
    case 1146: return "42S02";
    case 1054: return "42S22";
    case 1050: return "42S01";
    case 1406: return "22001";
    case 1264: return "22003";
    default: return "HY000";
  }
}

// Escape a value for a JSON string literal.
std::string json_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      default:
        if (c < 0x20) {
          char b[8];
          std::snprintf(b, sizeof(b), "\\u%04x", c);
          out += b;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out;
}

// True when the text is a plain number; used to give JSON output real types.
bool looks_numeric(const std::string &s) {
  if (s.empty()) return false;
  size_t i = 0;
  if (s[i] == '-' || s[i] == '+') ++i;
  bool digits = false, dot = false, expo = false;
  for (; i < s.size(); ++i) {
    char c = s[i];
    if (std::isdigit(static_cast<unsigned char>(c))) {
      digits = true;
      continue;
    }
    if (c == '.' && !dot && !expo) {
      dot = true;
      continue;
    }
    if ((c == 'e' || c == 'E') && digits && !expo) {
      expo = true;
      if (i + 1 < s.size() && (s[i + 1] == '-' || s[i + 1] == '+')) ++i;
      continue;
    }
    return false;
  }
  return digits;
}

class Output {
 public:
  explicit Output(std::string mode) : mode_(std::move(mode)) {
    if (const char *e = std::getenv("MVDB_DML_STATUS")) {
      if (std::strcmp(e, "0") == 0) dml_status_ = false;
    }
    if (const char *e = std::getenv("MVDB_HEADERS")) {
      if (std::strcmp(e, "0") == 0) headers_ = false;
    }
  }

  void ready() {
    if (mode_ == "json") {
      std::fputs("{\"status\":\"ready\"}\n", stdout);
      std::fflush(stdout);
      return;
    }
    if (mode_ == "framed" || mode_ == "mysql") {
      const char *e = std::getenv("MVDB_READY");
      if (e && (std::strcmp(e, "none") == 0 || e[0] == '\0')) return;
      std::fputs(e ? e : "READY", stdout);
      std::fputc('\n', stdout);
      std::fflush(stdout);
    }
  }

  void bye() {
    if (mode_ == "json") {
      std::fputs("{\"status\":\"bye\"}\n", stdout);
      std::fflush(stdout);
      return;
    }
    if (mode_ == "framed" || mode_ == "mysql") {
      const char *e = std::getenv("MVDB_BYE");
      if (e && (std::strcmp(e, "none") == 0 || e[0] == '\0')) return;
      std::fputs(e ? e : "BYE", stdout);
      std::fputc('\n', stdout);
      std::fflush(stdout);
    }
  }

  void result(const ResultSet &rs) {
    if (mode_ == "framed") {
      std::printf("RESULT %zu %zu\n", rs.columns.size(), rs.rows.size());
      if (headers_) write_row(rs.columns);
      for (const auto &r : rs.rows) write_row(r);
    } else if (mode_ == "mysql") {
      if (rs.query) {
        if (headers_) write_row(rs.columns);
        for (const auto &r : rs.rows) write_row(r);
      } else {
        std::fputs("OK\n", stdout);
      }
    } else if (mode_ == "json") {
      std::fputs("{\"status\":\"result\",\"columns\":[", stdout);
      for (size_t i = 0; i < rs.columns.size(); ++i) {
        if (i) std::fputc(',', stdout);
        std::printf("\"%s\"", json_escape(rs.columns[i]).c_str());
      }
      std::fputs("],\"rows\":[", stdout);
      for (size_t i = 0; i < rs.rows.size(); ++i) {
        if (i) std::fputc(',', stdout);
        std::fputc('[', stdout);
        for (size_t j = 0; j < rs.rows[i].size(); ++j) {
          if (j) std::fputc(',', stdout);
          const std::string &v = rs.rows[i][j];
          if (looks_numeric(v))
            std::fputs(v.c_str(), stdout);
          else
            std::printf("\"%s\"", json_escape(v).c_str());
        }
        std::fputc(']', stdout);
      }
      std::fputs("]}\n", stdout);
    } else if (mode_ == "tsv") {
      if (rs.query) {
        if (headers_) write_row(rs.columns);
        for (const auto &r : rs.rows) write_row(r);
      }
    }
    std::fflush(stdout);
  }

  void ok(i64 affected) {
    if (!dml_status_) return;
    if (mode_ == "framed") {
      std::printf("OK %lld\n", static_cast<long long>(affected));
    } else if (mode_ == "mysql") {
      std::fputs("OK\n", stdout);
    } else if (mode_ == "json") {
      std::printf("{\"status\":\"ok\",\"affected_rows\":%lld}\n", static_cast<long long>(affected));
    }
    std::fflush(stdout);
  }

  void error(const Error &e) {
    std::string msg = e.message;
    for (char &c : msg)
      if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    if (mode_ == "framed") {
      std::printf("ERROR %d %s\n", e.code, msg.c_str());
    } else if (mode_ == "mysql") {
      std::printf("ERROR %d (%s): %s\n", e.code, sqlstate_for(e.code), msg.c_str());
    } else if (mode_ == "json") {
      std::printf("{\"status\":\"error\",\"code\":%d,\"message\":\"%s\"}\n", e.code,
                  json_escape(msg).c_str());
    } else {
      std::printf("ERROR %d %s\n", e.code, msg.c_str());
    }
    std::fflush(stdout);
  }

 private:
  void write_row(const std::vector<std::string> &row) {
    for (size_t i = 0; i < row.size(); ++i) {
      if (i) std::fputc('\t', stdout);
      std::string e = escape_field(row[i]);
      std::fwrite(e.data(), 1, e.size(), stdout);
    }
    std::fputc('\n', stdout);
  }

  std::string mode_;
  bool dml_status_ = true;
  bool headers_ = true;
};

std::string g_data_dir = "/data";
std::string g_protocol = "framed";

std::string arg_value(int argc, char **argv, const char *name, const std::string &def) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
  }
  return def;
}

}  // namespace

}  // namespace mvdb

int main(int argc, char **argv) {
  using namespace mvdb;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      std::fprintf(stderr,
                   "MiniVectorDB\n"
                   "  --data-dir DIR   data directory (default /data)\n"
                   "  --protocol P     framed|mysql|tsv|json (default framed)\n"
                   "  statements are terminated by ';' or by end of line\n");
      return 0;
    }
  }
  g_data_dir = arg_value(argc, argv, "--data-dir", "/data");
  g_protocol = arg_value(argc, argv, "--protocol", "framed");
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a.rfind("--data-dir=", 0) == 0) g_data_dir = a.substr(11);
    else if (a.rfind("--protocol=", 0) == 0) g_protocol = a.substr(11);
  }
  if (const char *p = std::getenv("MVDB_PROTOCOL")) g_protocol = p;
  if (const char *d = std::getenv("MVDB_DATA_DIR")) g_data_dir = d;
  if (g_protocol == "plain") g_protocol = "tsv";
  if (g_protocol != "framed" && g_protocol != "mysql" && g_protocol != "tsv" &&
      g_protocol != "json")
    g_protocol = "framed";

  if (::pipe(g_signal_pipe) != 0) {
    std::fprintf(stderr, "cannot create signal pipe: %s\n", std::strerror(errno));
    return 1;
  }
  struct sigaction sa;
  std::memset(&sa, 0, sizeof(sa));
  sa.sa_handler = signal_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, nullptr);
  sigaction(SIGTERM, &sa, nullptr);
  sigaction(SIGHUP, &sa, nullptr);
  // A closed stdout must not kill the process before the shutdown path runs.
  signal(SIGPIPE, SIG_IGN);

  Output out(g_protocol);
  std::unique_ptr<Database> db;
  try {
    db = std::make_unique<Database>(g_data_dir);
    db->open();
  } catch (const Error &e) {
    std::fprintf(stderr, "startup failed: %d %s\n", e.code, e.message.c_str());
    return 1;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "startup failed: %s\n", e.what());
    return 1;
  }

  out.ready();
  std::string buf;
  bool quitting = false;
  bool eof = false;
  const size_t kMaxBuffer = 64u * 1024u * 1024u;

  auto run_text = [&](const std::string &stmt) -> bool {
    std::string t = lower_trim(stmt);
    if (t == "quit" || t == "exit" || t == "\\q" || t == ".quit" || t == ".exit")
      return true;
    if (t.empty() || t == ";") {
      return false;
    }
    try {
      Parser parser(stmt);
      Statement s = parser.parse();
      if (s.kind == StmtKind::QUIT) return true;
      if (s.kind == StmtKind::EMPTY) {
        return false;
      }
      ResultSet rs = db->execute(s);
      if (rs.query) out.result(rs);
      else out.ok(rs.affected);
    } catch (const Error &e) {
      out.error(e);
    } catch (const std::exception &e) {
      out.error(Error{1105, e.what()});
    }
    return false;
  };

  while (!g_stop && !quitting) {
    struct pollfd fds[2];
    fds[0].fd = 0;
    fds[0].events = POLLIN;
    fds[1].fd = g_signal_pipe[0];
    fds[1].events = POLLIN;
    int pr = ::poll(fds, 2, 1000);
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (g_stop) break;
    if (fds[1].revents & POLLIN) {
      char tmp[64];
      while (::read(g_signal_pipe[0], tmp, sizeof(tmp)) > 0) {
      }
      break;
    }
    if (!(fds[0].revents & (POLLIN | POLLHUP | POLLERR))) continue;
    char chunk[65536];
    ssize_t n = ::read(0, chunk, sizeof(chunk));
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (n == 0) {
      eof = true;
      if (buf.empty()) break;
    } else {
      buf.append(chunk, static_cast<size_t>(n));
      if (buf.size() >= 3 && static_cast<unsigned char>(buf[0]) == 0xEF &&
          static_cast<unsigned char>(buf[1]) == 0xBB &&
          static_cast<unsigned char>(buf[2]) == 0xBF)
        buf.erase(0, 3);
      if (buf.size() > kMaxBuffer) {
        out.error(Error{ER_PARSE, "input statement too large"});
        buf.clear();
        continue;
      }
    }

    for (;;) {
      std::string stmt;
      if (!next_statement(buf, stmt)) break;
      if (run_text(stmt)) {
        quitting = true;
        break;
      }
    }

    if (eof) {
      std::string rest = trim(buf);
      if (!rest.empty() && !quitting) {
        if (run_text(rest)) quitting = true;
      }
      break;
    }
  }

  try {
    db->close();
  } catch (const Error &e) {
    std::fprintf(stderr, "shutdown failed: %d %s\n", e.code, e.message.c_str());
  } catch (const std::exception &e) {
    std::fprintf(stderr, "shutdown failed: %s\n", e.what());
  }
  out.bye();
  return 0;
}
