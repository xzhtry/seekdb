#include "sql.h"

#include <charconv>
#include <cmath>

namespace mvdb {

ExprPtr clone_expr(const Expr &e) {
  auto n = std::make_unique<Expr>();
  n->kind = e.kind;
  n->i = e.i;
  n->d = e.d;
  n->s = e.s;
  n->nums = e.nums;
  n->fnums = e.fnums;
  n->text = e.text;
  n->args.reserve(e.args.size());
  for (const auto &a : e.args) n->args.push_back(clone_expr(*a));
  return n;
}

namespace {

bool is_ident_start(unsigned char c) {
  return std::isalpha(c) || c == '_' || c == '$' || c >= 0x80;
}
bool is_ident_char(unsigned char c) {
  return std::isalnum(c) || c == '_' || c == '$' || c >= 0x80;
}

std::string unescape_sql_string(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' && i + 1 < s.size()) {
      char c = s[++i];
      switch (c) {
        case 'n': out.push_back('\n'); break;
        case 't': out.push_back('\t'); break;
        case 'r': out.push_back('\r'); break;
        case '0': out.push_back('\0'); break;
        case 'b': out.push_back('\b'); break;
        case 'Z': out.push_back('\x1a'); break;
        case '\\': out.push_back('\\'); break;
        case '\'': out.push_back('\''); break;
        case '"': out.push_back('"'); break;
        default: out.push_back(c); break;
      }
    } else {
      out.push_back(s[i]);
    }
  }
  return out;
}

}  // namespace

Lexer::Lexer(std::string sql) : sql_(std::move(sql)) {}

std::vector<Token> Lexer::tokenize() {
  std::vector<Token> out;
  const size_t n = sql_.size();
  while (pos_ < n) {
    unsigned char c = static_cast<unsigned char>(sql_[pos_]);
    if (std::isspace(c)) {
      ++pos_;
      continue;
    }
    // -- comment
    if (c == '-' && pos_ + 1 < n && sql_[pos_ + 1] == '-') {
      size_t e = sql_.find('\n', pos_);
      if (e == std::string::npos) e = n;
      comments_.push_back(trim(std::string_view(sql_).substr(pos_ + 2, e - pos_ - 2)));
      pos_ = e;
      continue;
    }
    if (c == '#') {
      size_t e = sql_.find('\n', pos_);
      if (e == std::string::npos) e = n;
      comments_.push_back(trim(std::string_view(sql_).substr(pos_ + 1, e - pos_ - 1)));
      pos_ = e;
      continue;
    }
    if (c == '/' && pos_ + 1 < n && sql_[pos_ + 1] == '*') {
      size_t e = sql_.find("*/", pos_ + 2);
      std::string body;
      if (e == std::string::npos) {
        body = std::string(sql_.substr(pos_ + 2));
        pos_ = n;
      } else {
        body = std::string(sql_.substr(pos_ + 2, e - pos_ - 2));
        pos_ = e + 2;
      }
      comments_.push_back(trim(body));
      continue;
    }
    Token t;
    t.pos = pos_;
    if (is_ident_start(c)) {
      size_t b = pos_;
      while (pos_ < n && is_ident_char(static_cast<unsigned char>(sql_[pos_]))) ++pos_;
      t.type = Token::Type::IDENT;
      t.text = sql_.substr(b, pos_ - b);
      out.push_back(std::move(t));
      continue;
    }
    if (c == '`') {
      ++pos_;
      std::string val;
      while (pos_ < n) {
        if (sql_[pos_] == '`') {
          if (pos_ + 1 < n && sql_[pos_ + 1] == '`') {
            val.push_back('`');
            pos_ += 2;
            continue;
          }
          break;
        }
        val.push_back(sql_[pos_++]);
      }
      if (pos_ >= n) fail(ER_PARSE, "unterminated quoted identifier");
      ++pos_;  // closing backtick
      t.type = Token::Type::IDENT;
      t.text = std::move(val);
      t.quoted = true;
      out.push_back(std::move(t));
      continue;
    }
    if (c == '\'' || c == '"') {
      char quote = static_cast<char>(c);
      ++pos_;
      std::string raw;
      bool closed = false;
      while (pos_ < n) {
        char ch = sql_[pos_];
        if (ch == quote) {
          if (pos_ + 1 < n && sql_[pos_ + 1] == quote) {
            raw.push_back(quote);
            pos_ += 2;
            continue;
          }
          ++pos_;
          closed = true;
          break;
        }
        if (ch == '\\' && pos_ + 1 < n) {
          raw.push_back(ch);
          raw.push_back(sql_[pos_ + 1]);
          pos_ += 2;
          continue;
        }
        raw.push_back(ch);
        ++pos_;
      }
      if (!closed) fail(ER_PARSE, "unterminated string literal");
      t.type = Token::Type::STRING;
      t.text = unescape_sql_string(raw);
      out.push_back(std::move(t));
      continue;
    }
    if (std::isdigit(c) || (c == '.' && pos_ + 1 < n && std::isdigit(static_cast<unsigned char>(sql_[pos_ + 1])))) {
      size_t b = pos_;
      bool dot = false, exp = false;
      if (c == '.') { dot = true; ++pos_; }
      while (pos_ < n) {
        unsigned char d = static_cast<unsigned char>(sql_[pos_]);
        if (std::isdigit(d)) {
          ++pos_;
        } else if (d == '.' && !dot && !exp) {
          dot = true;
          ++pos_;
        } else if ((d == 'e' || d == 'E') && !exp) {
          exp = true;
          ++pos_;
          if (pos_ < n && (sql_[pos_] == '+' || sql_[pos_] == '-')) ++pos_;
        } else {
          break;
        }
      }
      t.type = Token::Type::NUMBER;
      t.text = sql_.substr(b, pos_ - b);
      t.is_float = dot || exp;
      out.push_back(std::move(t));
      continue;
    }
    // punctuation
    t.type = Token::Type::PUNCT;
    t.text = std::string(1, static_cast<char>(c));
    ++pos_;
    out.push_back(std::move(t));
  }
  Token end;
  end.type = Token::Type::END;
  end.pos = n;
  out.push_back(std::move(end));
  return out;
}

std::vector<double> parse_vector_text(std::string_view s, bool &ok) {
  ok = false;
  std::vector<double> out;
  size_t i = 0;
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
  if (i >= s.size() || s[i] != '[') return out;
  ++i;
  while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
  if (i < s.size() && s[i] == ']') {
    ++i;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    ok = (i == s.size());
    return out;
  }
  while (i < s.size()) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    size_t b = i;
    if (i < s.size() && s[i] == '+') ++i;
    if (i >= s.size()) return out;
    const char *nb = s.data() + i;
    const char *ne = s.data() + s.size();
    double v = 0;
    auto res = std::from_chars(nb, ne, v);
    if (res.ec != std::errc() || res.ptr == nb) return out;
    if (!std::isfinite(v)) return out;
    out.push_back(v);
    i = static_cast<size_t>(res.ptr - s.data());
    (void)b;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    if (i < s.size() && s[i] == ',') {
      ++i;
      continue;
    }
    if (i < s.size() && s[i] == ']') {
      ++i;
      while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
      if (i != s.size()) return out;
      ok = true;
      return out;
    }
    return out;
  }
  return out;
}

Parser::Parser(std::string sql) : sql_(std::move(sql)) {
  Lexer lex(sql_);
  tokens_ = lex.tokenize();
  comments_ = lex.comments();
}

const Token &Parser::peek(size_t n) const {
  size_t i = pos_ + n;
  if (i >= tokens_.size()) return tokens_.back();
  return tokens_[i];
}

Token Parser::next() {
  Token t = peek();
  if (pos_ < tokens_.size()) ++pos_;
  return t;
}

bool Parser::at_end() const { return peek().type == Token::Type::END; }

bool Parser::is_kw(std::string_view k, size_t n) const {
  const Token &t = peek(n);
  return t.type == Token::Type::IDENT && !t.quoted && iequals(t.text, k);
}

bool Parser::accept_kw(std::string_view k) {
  if (is_kw(k)) {
    next();
    return true;
  }
  return false;
}

void Parser::expect_kw(std::string_view k) {
  if (!accept_kw(k)) {
    fail(ER_PARSE, "expected keyword " + std::string(k) + " near '" + peek().text + "'");
  }
}

bool Parser::accept_punct(char c) {
  const Token &t = peek();
  if (t.type == Token::Type::PUNCT && t.text.size() == 1 && t.text[0] == c) {
    next();
    return true;
  }
  return false;
}

void Parser::expect_punct(char c) {
  if (!accept_punct(c)) {
    fail(ER_PARSE, std::string("expected '") + c + "' near '" + peek().text + "'");
  }
}

std::string Parser::parse_ident(const char *what) {
  const Token &t = peek();
  if (t.type != Token::Type::IDENT) {
    fail(ER_PARSE, std::string("expected ") + what + " near '" + t.text + "'");
  }
  if (!t.quoted && is_clause_keyword(t)) {
    fail(ER_PARSE, std::string("expected ") + what + " near '" + t.text + "'");
  }
  return next().text;
}

bool Parser::is_clause_keyword(const Token &t) const {
  if (t.type != Token::Type::IDENT || t.quoted) return false;
  static const std::set<std::string> kws = {
      "from", "where", "order", "limit", "offset", "as", "and", "or", "asc", "desc",
      "values", "set", "into", "on", "using", "with", "primary", "key",
      "not", "null", "if", "exists", "by", "group", "having", "join", "inner",
      "left", "right", "outer", "union", "select", "insert", "update", "delete",
      "create", "drop", "table", "index", "vector", "begin", "commit", "rollback",
      "show", "describe", "desc", "quit", "exit", "for", "share", "lock", "in",
      "is", "like", "between", "distinct", "all", "database", "session", "global"};
  return kws.count(to_lower(t.text)) > 0;
}

bool Parser::is_condition_end(const Token &t) const {
  if (t.type == Token::Type::END) return true;
  if (t.type == Token::Type::PUNCT) {
    if (t.text == ")" || t.text == "," || t.text == ";" || t.text == "]") return true;
    return false;
  }
  if (t.type == Token::Type::IDENT && !t.quoted) {
    std::string k = to_lower(t.text);
    if (k == "order" || k == "limit" || k == "offset" || k == "group" ||
        k == "having" || k == "asc" || k == "desc")
      return true;
  }
  return false;
}

Statement Parser::parse() {
  Statement st;
  for (const auto &c : comments_) {
    std::string lc = to_lower(c);
    if (lc.find("ann") != std::string::npos && lc.find("exact") == std::string::npos) {
      st.hint = "ann";
    } else if (lc.find("exact") != std::string::npos) {
      st.hint = "exact";
    }
  }
  while (accept_punct(';')) {
  }
  if (at_end()) {
    st.kind = StmtKind::EMPTY;
    return st;
  }
  Statement body = parse_statement();
  body.hint = st.hint;
  while (accept_punct(';')) {
  }
  if (!at_end()) {
    fail(ER_PARSE, "unexpected token '" + peek().text + "'");
  }
  return body;
}

Statement Parser::parse_statement() {
  Statement st;
  if (is_kw("select")) {
    st.kind = StmtKind::SELECT;
    st.select = parse_select();
  } else if (is_kw("insert")) {
    st.kind = StmtKind::INSERT;
    st.insert = parse_insert();
  } else if (is_kw("update")) {
    st.kind = StmtKind::UPDATE;
    st.update = parse_update();
  } else if (is_kw("delete")) {
    st.kind = StmtKind::DELETE;
    st.del = parse_delete();
  } else if (is_kw("create")) {
    next();
    if (accept_kw("table")) {
      st.kind = StmtKind::CREATE_TABLE;
      st.create_table = parse_create_table();
    } else if (accept_kw("vector")) {
      expect_kw("index");
      st.kind = StmtKind::CREATE_INDEX;
      st.create_index = parse_create_index();
    } else if (accept_kw("index")) {
      st.kind = StmtKind::CREATE_INDEX;
      st.create_index = parse_create_index();
    } else {
      fail(ER_UNSUPPORTED, "only CREATE TABLE / CREATE VECTOR INDEX are supported");
    }
  } else if (is_kw("drop")) {
    next();
    if (accept_kw("table")) {
      st.kind = StmtKind::DROP_TABLE;
      st.drop_table = parse_drop_table();
    } else if (accept_kw("index") || (accept_kw("vector") && accept_kw("index"))) {
      st.kind = StmtKind::DROP_INDEX;
      st.drop_index = parse_drop_index();
    } else {
      fail(ER_UNSUPPORTED, "only DROP TABLE / DROP INDEX are supported");
    }
  } else if (is_kw("begin")) {
    next();
    accept_kw("work");
    accept_kw("transaction");
    st.kind = StmtKind::BEGIN;
  } else if (is_kw("start")) {
    next();
    expect_kw("transaction");
    st.kind = StmtKind::BEGIN;
  } else if (is_kw("commit")) {
    next();
    accept_kw("work");
    st.kind = StmtKind::COMMIT;
  } else if (is_kw("rollback")) {
    next();
    accept_kw("work");
    st.kind = StmtKind::ROLLBACK;
  } else if (is_kw("set")) {
    st.kind = StmtKind::SET;
    st.set = parse_set();
  } else if (is_kw("show")) {
    next();
    if (accept_kw("tables")) {
      st.kind = StmtKind::SHOW_TABLES;
    } else {
      fail(ER_UNSUPPORTED, "SHOW is not supported except SHOW TABLES");
    }
  } else if (is_kw("describe") || is_kw("desc")) {
    next();
    st.kind = StmtKind::DESCRIBE;
    st.describe.table = parse_ident("table name");
  } else if (is_kw("quit") || is_kw("exit") || is_kw("shutdown")) {
    next();
    st.kind = StmtKind::QUIT;
  } else {
    fail(ER_PARSE, "unsupported statement near '" + peek().text + "'");
  }
  return st;
}

SelectStmt Parser::parse_select() {
  expect_kw("select");
  SelectStmt s;
  if (accept_punct('*')) {
    SelectItem it;
    it.star = true;
    s.items.push_back(std::move(it));
  } else {
    for (;;) {
      SelectItem it;
      it.expr = parse_expr();
      if (accept_kw("as")) {
        if (peek().type == Token::Type::STRING) it.alias = next().text;
        else it.alias = parse_ident("alias");
      } else if (peek().type == Token::Type::IDENT && !is_clause_keyword(peek())) {
        // Bare alias.  Only accept it when it is a plain identifier and not
        // a clause keyword.
        it.alias = next().text;
      }
      s.items.push_back(std::move(it));
      if (!accept_punct(',')) break;
    }
  }
  expect_kw("from");
  s.table = parse_ident("table name");
  if (accept_kw("as")) {
    parse_ident("table alias");
  } else if (peek().type == Token::Type::IDENT && !is_clause_keyword(peek())) {
    next();  // bare table alias, not used for name resolution
  }
  if (accept_kw("where")) {
    s.where = parse_condition();
  }
  if (accept_kw("order")) {
    expect_kw("by");
    for (;;) {
      OrderItem oi;
      oi.expr = parse_expr();
      if (accept_kw("desc")) oi.desc = true;
      else accept_kw("asc");
      s.order.push_back(std::move(oi));
      if (!accept_punct(',')) break;
    }
  }
  if (accept_kw("limit")) {
    const Token &t = peek();
    if (t.type != Token::Type::NUMBER || t.is_float) {
      fail(ER_PARSE, "LIMIT expects an integer");
    }
    std::string a = next().text;
    i64 v = 0;
    if (!parse_i64(a, v) || v < 0) fail(ER_PARSE, "invalid LIMIT value");
    if (accept_punct(',')) {
      s.offset = v;
      const Token &t2 = peek();
      if (t2.type != Token::Type::NUMBER || t2.is_float) fail(ER_PARSE, "LIMIT expects an integer");
      std::string b = next().text;
      if (!parse_i64(b, s.limit) || s.limit < 0) fail(ER_PARSE, "invalid LIMIT value");
    } else {
      s.limit = v;
      if (accept_kw("offset")) {
        const Token &t2 = peek();
        if (t2.type != Token::Type::NUMBER || t2.is_float) fail(ER_PARSE, "OFFSET expects an integer");
        std::string b = next().text;
        if (!parse_i64(b, s.offset) || s.offset < 0) fail(ER_PARSE, "invalid OFFSET value");
      }
    }
  }
  return s;
}

InsertStmt Parser::parse_insert() {
  expect_kw("insert");
  expect_kw("into");
  InsertStmt s;
  s.table = parse_ident("table name");
  if (accept_punct('(')) {
    for (;;) {
      s.columns.push_back(parse_ident("column name"));
      if (!accept_punct(',')) break;
    }
    expect_punct(')');
  }
  expect_kw("values");
  for (;;) {
    expect_punct('(');
    std::vector<ExprPtr> row;
    if (!accept_punct(')')) {
      for (;;) {
        row.push_back(parse_expr());
        if (!accept_punct(',')) break;
      }
      expect_punct(')');
    }
    s.rows.push_back(std::move(row));
    if (!accept_punct(',')) break;
  }
  return s;
}

UpdateStmt Parser::parse_update() {
  expect_kw("update");
  UpdateStmt s;
  s.table = parse_ident("table name");
  expect_kw("set");
  for (;;) {
    std::string col = parse_ident("column name");
    expect_punct('=');
    ExprPtr e = parse_expr();
    s.sets.emplace_back(std::move(col), std::move(e));
    if (!accept_punct(',')) break;
  }
  if (accept_kw("where")) s.where = parse_condition();
  return s;
}

DeleteStmt Parser::parse_delete() {
  expect_kw("delete");
  expect_kw("from");
  DeleteStmt s;
  s.table = parse_ident("table name");
  if (accept_kw("where")) s.where = parse_condition();
  return s;
}

CreateTableStmt Parser::parse_create_table() {
  CreateTableStmt s;
  if (accept_kw("if")) {
    expect_kw("not");
    expect_kw("exists");
    s.if_not_exists = true;
  }
  s.table = parse_ident("table name");
  expect_punct('(');
  for (;;) {
    if (is_kw("primary")) {
      next();
      expect_kw("key");
      expect_punct('(');
      for (;;) {
        s.primary_key.push_back(parse_ident("column name"));
        if (!accept_punct(',')) break;
      }
      expect_punct(')');
    } else if (is_kw("key") || is_kw("index") || is_kw("unique")) {
      // Ignore secondary key/index declarations.
      while (!at_end() && !(peek().type == Token::Type::PUNCT && peek().text == ",") &&
             !(peek().type == Token::Type::PUNCT && peek().text == ")")) {
        next();
      }
    } else {
      ColumnDef c;
      c.name = parse_ident("column name");
      const Token &tytok = peek();
      if (tytok.type != Token::Type::IDENT)
        fail(ER_PARSE, "expected column type near '" + tytok.text + "'");
      std::string ty = to_lower(next().text);
      if (ty == "bigint" || ty == "int" || ty == "integer") {
        c.type = ColType::BIGINT;
        c.param = 0;
      } else if (ty == "varchar") {
        c.type = ColType::VARCHAR;
        expect_punct('(');
        const Token &t = peek();
        if (t.type != Token::Type::NUMBER || t.is_float) fail(ER_PARSE, "VARCHAR expects a length");
        i64 v = 0;
        if (!parse_i64(next().text, v) || v <= 0 || v > 1000000)
          fail(ER_PARSE, "invalid VARCHAR length");
        c.param = static_cast<u32>(v);
        expect_punct(')');
      } else if (ty == "vector") {
        c.type = ColType::VECTOR;
        expect_punct('(');
        const Token &t = peek();
        if (t.type != Token::Type::NUMBER || t.is_float) fail(ER_PARSE, "VECTOR expects a dimension");
        i64 v = 0;
        if (!parse_i64(next().text, v) || v <= 0 || v > 65536)
          fail(ER_PARSE, "invalid VECTOR dimension");
        c.param = static_cast<u32>(v);
        expect_punct(')');
      } else {
        fail(ER_UNSUPPORTED, "unsupported column type '" + ty + "'");
      }
      // Column constraints.
      for (;;) {
        if (accept_kw("primary")) {
          expect_kw("key");
          c.primary = true;
        } else if (accept_kw("not")) {
          expect_kw("null");
        } else if (accept_kw("unique")) {
          // uniqueness not required; ignore
        } else if (accept_kw("default")) {
          // No NULL/default support; consume the value so parsing succeeds but
          // report a clear error at execution time.
          parse_expr();
        } else {
          break;
        }
      }
      s.columns.push_back(std::move(c));
    }
    if (accept_punct(',')) continue;
    break;
  }
  expect_punct(')');
  return s;
}

CreateIndexStmt Parser::parse_create_index() {
  CreateIndexStmt s;
  if (accept_kw("if")) {
    expect_kw("not");
    expect_kw("exists");
    s.if_not_exists = true;
  }
  s.index = parse_ident("index name");
  expect_kw("on");
  s.table = parse_ident("table name");
  expect_punct('(');
  s.column = parse_ident("column name");
  expect_punct(')');
  // Optional USING / WITH clauses are accepted and ignored.
  while (!at_end() && !(peek().type == Token::Type::PUNCT && peek().text == ";")) next();
  return s;
}

DropTableStmt Parser::parse_drop_table() {
  DropTableStmt s;
  if (accept_kw("if")) {
    expect_kw("exists");
    s.if_exists = true;
  }
  s.table = parse_ident("table name");
  return s;
}

DropIndexStmt Parser::parse_drop_index() {
  DropIndexStmt s;
  if (accept_kw("if")) {
    expect_kw("exists");
    s.if_exists = true;
  }
  s.index = parse_ident("index name");
  expect_kw("on");
  s.table = parse_ident("table name");
  return s;
}

SetStmt Parser::parse_set() {
  expect_kw("set");
  SetStmt s;
  accept_kw("session");
  accept_kw("global");
  if (accept_punct('@')) accept_punct('@');
  s.name = parse_ident("variable name");
  expect_punct('=');
  s.value = parse_expr();
  return s;
}

DescribeStmt Parser::parse_describe() {
  DescribeStmt s;
  s.table = parse_ident("table name");
  return s;
}

ExprPtr Parser::parse_expr() {
  return parse_add();
}

ExprPtr Parser::parse_add() {
  ExprPtr left = parse_mul();
  for (;;) {
    if (accept_punct('+')) {
      ExprPtr e = make_expr(ExprKind::ADD);
      e->args.push_back(std::move(left));
      e->args.push_back(parse_mul());
      left = std::move(e);
    } else if (accept_punct('-')) {
      ExprPtr e = make_expr(ExprKind::SUB);
      e->args.push_back(std::move(left));
      e->args.push_back(parse_mul());
      left = std::move(e);
    } else {
      break;
    }
  }
  return left;
}

ExprPtr Parser::parse_mul() {
  ExprPtr left = parse_unary();
  for (;;) {
    if (accept_punct('*')) {
      ExprPtr e = make_expr(ExprKind::MUL);
      e->args.push_back(std::move(left));
      e->args.push_back(parse_unary());
      left = std::move(e);
    } else if (accept_punct('/')) {
      ExprPtr e = make_expr(ExprKind::DIV);
      e->args.push_back(std::move(left));
      e->args.push_back(parse_unary());
      left = std::move(e);
    } else {
      break;
    }
  }
  return left;
}

ExprPtr Parser::parse_unary() {
  if (peek().type == Token::Type::PUNCT && peek().text == "-") {
    // Special case -9223372036854775808: the magnitude alone does not fit in
    // int64, but the signed value does.
    if (peek(1).type == Token::Type::NUMBER && !peek(1).is_float) {
      std::string s = "-" + peek(1).text;
      i64 v = 0;
      if (parse_i64(s, v)) {
        next();
        next();
        ExprPtr e = make_expr(ExprKind::INT_LITERAL);
        e->i = v;
        e->text = s;
        return e;
      }
    }
    next();
    ExprPtr e = make_expr(ExprKind::NEG);
    e->args.push_back(parse_unary());
    return e;
  }
  if (accept_punct('+')) return parse_unary();
  return parse_primary();
}

ExprPtr Parser::parse_primary() {
  const Token &t = peek();
  if (t.type == Token::Type::NUMBER) {
    std::string s = next().text;
    if (t.is_float) {
      double d = 0;
      if (!parse_double(s, d)) fail(ER_OUT_OF_RANGE, "invalid numeric literal");
      ExprPtr e = make_expr(ExprKind::DOUBLE_LITERAL);
      e->d = d;
      e->text = s;
      return e;
    }
    i64 v = 0;
    if (!parse_i64(s, v)) fail(ER_OUT_OF_RANGE, "integer literal out of range");
    ExprPtr e = make_expr(ExprKind::INT_LITERAL);
    e->i = v;
    e->text = s;
    return e;
  }
  if (t.type == Token::Type::STRING) {
    std::string s = next().text;
    ExprPtr e = make_expr(ExprKind::STRING_LITERAL);
    e->s = std::move(s);
    e->text = "'" + e->s + "'";
    return e;
  }
  if (t.type == Token::Type::PUNCT && t.text == "(") {
    next();
    ExprPtr e = parse_expr();
    expect_punct(')');
    return e;
  }
  if (t.type == Token::Type::PUNCT && t.text == "[") {
    // Parse a bare vector literal: [1,2,3]
    size_t start = t.pos;
    std::string raw;
    int depth = 0;
    while (!at_end()) {
      if (peek().type == Token::Type::PUNCT && peek().text == "[") depth++;
      if (peek().type == Token::Type::PUNCT && peek().text == "]") {
        depth--;
        if (depth == 0) {
          raw += next().text;
          break;
        }
      }
      raw += next().text;
    }
    (void)start;
    bool ok = false;
    std::vector<double> nums = parse_vector_text(raw, ok);
    if (!ok) fail(ER_PARSE, "invalid vector literal");
    ExprPtr e = make_expr(ExprKind::VECTOR_LITERAL);
    e->nums = std::move(nums);
    e->text = raw;
    return e;
  }
  if (t.type == Token::Type::IDENT) {
    std::string name = t.text;
    if (is_clause_keyword(t)) {
      fail(ER_PARSE, "expected expression near '" + name + "'");
    }
    next();
    if (accept_punct('.')) {
      // Qualified column: table.column.  The table qualifier is validated by
      // the executor only implicitly; the column name is what matters here.
      name = parse_ident("column name");
    }
    if (accept_punct('(')) {
      std::vector<ExprPtr> args;
      if (!accept_punct(')')) {
        for (;;) {
          args.push_back(parse_expr());
          if (!accept_punct(',')) break;
        }
        expect_punct(')');
      }
      ExprPtr e;
      if (iequals(name, "l2_distance")) {
        if (args.size() != 2) fail(ER_PARSE, "L2_DISTANCE expects 2 arguments");
        e = make_expr(ExprKind::L2_DISTANCE);
        e->text = "L2_DISTANCE";
      } else {
        e = make_expr(ExprKind::FUNC);
        e->s = to_lower(name);
        e->text = name;
      }
      e->args = std::move(args);
      if (e->kind == ExprKind::L2_DISTANCE) {
        e->text = "L2_DISTANCE(" + e->args[0]->text + ", " + e->args[1]->text + ")";
      }
      return e;
    }
    ExprPtr e = make_expr(ExprKind::COLUMN);
    e->s = name;
    e->text = name;
    return e;
  }
  fail(ER_PARSE, "expected expression near '" + t.text + "'");
}

CondPtr Parser::parse_condition() { return parse_or_condition(); }

CondPtr Parser::parse_or_condition() {
  CondPtr left = parse_and_condition();
  while (accept_kw("or")) {
    auto c = std::make_unique<Cond>();
    c->kind = CondKind::OR;
    c->children.push_back(std::move(left));
    c->children.push_back(parse_and_condition());
    left = std::move(c);
  }
  return left;
}

CondPtr Parser::parse_and_condition() {
  CondPtr left = parse_not_condition();
  while (accept_kw("and")) {
    auto c = std::make_unique<Cond>();
    c->kind = CondKind::AND;
    c->children.push_back(std::move(left));
    c->children.push_back(parse_not_condition());
    left = std::move(c);
  }
  return left;
}

CondPtr Parser::parse_not_condition() {
  if (accept_kw("not")) {
    auto c = std::make_unique<Cond>();
    c->kind = CondKind::NOT;
    c->children.push_back(parse_not_condition());
    return c;
  }
  return parse_condition_atom();
}

CondPtr Parser::parse_condition_atom() {
  if (accept_punct('(')) {
    CondPtr c = parse_condition();
    expect_punct(')');
    return c;
  }
  ExprPtr left = parse_expr();
  CmpOp op;
  bool have = false;
  if (accept_punct('=')) {
    op = CmpOp::EQ;
    have = true;
  } else if (accept_punct('<')) {
    if (accept_punct('=')) op = CmpOp::LE;
    else if (accept_punct('>')) op = CmpOp::NE;
    else op = CmpOp::LT;
    have = true;
  } else if (accept_punct('>')) {
    if (accept_punct('=')) op = CmpOp::GE;
    else op = CmpOp::GT;
    have = true;
  } else if (accept_punct('!')) {
    expect_punct('=');
    op = CmpOp::NE;
    have = true;
  }
  if (!have) {
    fail(ER_PARSE, "expected comparison operator in WHERE clause");
  }
  auto c = std::make_unique<Cond>();
  c->kind = CondKind::COMPARE;
  c->op = op;
  c->left = std::move(left);
  c->right = parse_expr();
  return c;
}

}  // namespace mvdb
