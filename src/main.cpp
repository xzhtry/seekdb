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
      return true;
    }
    if (c == '(') ++paren;
    else if (c == ')' && paren > 0) --paren;
    else if (c == ';' && paren == 0) {
      stmt = buf.substr(0, i + 1);
      buf.erase(0, i + 1);
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
    if (mode_ == "framed" || mode_ == "mysql") {
      const char *e = std::getenv("MVDB_READY");
      if (e && (std::strcmp(e, "none") == 0 || e[0] == '\0')) return;
      std::fputs(e ? e : "READY", stdout);
      std::fputc('\n', stdout);
      std::fflush(stdout);
    }
  }

  void bye() {
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
                   "  --protocol P     framed|mysql|tsv (default framed)\n");
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
  if (g_protocol != "framed" && g_protocol != "mysql" && g_protocol != "tsv")
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
