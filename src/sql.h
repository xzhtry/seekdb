#pragma once

#include "mvdb.h"

namespace mvdb {

// ---------------------------------------------------------------------------
// Expression AST.
// ---------------------------------------------------------------------------
enum class ExprKind {
  INT_LITERAL,
  DOUBLE_LITERAL,
  STRING_LITERAL,
  VECTOR_LITERAL,
  COLUMN,
  L2_DISTANCE,
  ADD,
  SUB,
  MUL,
  DIV,
  NEG,
  FUNC
};

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct Expr {
  ExprKind kind = ExprKind::INT_LITERAL;
  i64 i = 0;
  double d = 0;
  std::string s;            // string literal / column name / function name
  std::vector<double> nums; // vector literal
  std::vector<float> fnums; // prepared query vector (float32)
  std::vector<ExprPtr> args;
  std::string text;         // original-ish text for column headers
};

inline ExprPtr make_expr(ExprKind k) {
  auto e = std::make_unique<Expr>();
  e->kind = k;
  return e;
}

ExprPtr clone_expr(const Expr &e);

// ---------------------------------------------------------------------------
// Conditions.
// ---------------------------------------------------------------------------
enum class CmpOp { EQ, NE, LT, LE, GT, GE };
enum class CondKind { COMPARE, AND, OR, NOT };

struct Cond;
using CondPtr = std::unique_ptr<Cond>;

struct Cond {
  CondKind kind = CondKind::COMPARE;
  CmpOp op = CmpOp::EQ;
  ExprPtr left;
  ExprPtr right;
  std::vector<CondPtr> children;
};

// ---------------------------------------------------------------------------
// Statements.
// ---------------------------------------------------------------------------
enum class StmtKind {
  EMPTY,
  SELECT,
  INSERT,
  UPDATE,
  DELETE,
  CREATE_TABLE,
  CREATE_INDEX,
  DROP_TABLE,
  DROP_INDEX,
  BEGIN,
  COMMIT,
  ROLLBACK,
  SET,
  SHOW_TABLES,
  DESCRIBE,
  QUIT
};

struct SelectItem {
  bool star = false;
  ExprPtr expr;
  std::string alias;
};

struct OrderItem {
  ExprPtr expr;
  bool desc = false;
};

struct SelectStmt {
  std::vector<SelectItem> items;
  std::string table;
  CondPtr where;
  std::vector<OrderItem> order;
  i64 limit = -1;
  i64 offset = 0;
};

struct InsertStmt {
  std::string table;
  std::vector<std::string> columns;
  std::vector<std::vector<ExprPtr>> rows;
};

struct UpdateStmt {
  std::string table;
  std::vector<std::pair<std::string, ExprPtr>> sets;
  CondPtr where;
};

struct DeleteStmt {
  std::string table;
  CondPtr where;
};

struct ColumnDef {
  std::string name;
  ColType type = ColType::BIGINT;
  u32 param = 0;
  bool primary = false;
};

struct CreateTableStmt {
  std::string table;
  bool if_not_exists = false;
  std::vector<ColumnDef> columns;
  std::vector<std::string> primary_key;
};

struct CreateIndexStmt {
  std::string index;
  std::string table;
  std::string column;
};

struct DropTableStmt {
  std::string table;
  bool if_exists = false;
};

struct DropIndexStmt {
  std::string index;
  std::string table;
  bool if_exists = false;
};

struct SetStmt {
  std::string name;
  ExprPtr value;
};

struct DescribeStmt {
  std::string table;
};

struct Statement {
  StmtKind kind = StmtKind::EMPTY;
  std::string hint;  // exact / ann from a leading comment
  SelectStmt select;
  InsertStmt insert;
  UpdateStmt update;
  DeleteStmt del;
  CreateTableStmt create_table;
  CreateIndexStmt create_index;
  DropTableStmt drop_table;
  DropIndexStmt drop_index;
  SetStmt set;
  DescribeStmt describe;
};

// ---------------------------------------------------------------------------
// Lexer / parser.
// ---------------------------------------------------------------------------
struct Token {
  enum class Type { END, IDENT, NUMBER, STRING, PUNCT } type = Type::END;
  std::string text;
  bool quoted = false;
  bool is_float = false;
  size_t pos = 0;
};

class Lexer {
 public:
  explicit Lexer(std::string sql);
  std::vector<Token> tokenize();
  const std::vector<std::string> &comments() const { return comments_; }

 private:
  std::string sql_;
  size_t pos_ = 0;
  std::vector<std::string> comments_;
};

class Parser {
 public:
  explicit Parser(std::string sql);
  Statement parse();

 private:
  std::vector<Token> tokens_;
  size_t pos_ = 0;
  std::vector<std::string> comments_;
  std::string sql_;

  const Token &peek(size_t n = 0) const;
  Token next();
  bool at_end() const;
  bool is_kw(std::string_view k, size_t n = 0) const;
  bool accept_kw(std::string_view k);
  void expect_kw(std::string_view k);
  bool accept_punct(char c);
  void expect_punct(char c);
  std::string parse_ident(const char *what = "identifier");

  Statement parse_statement();
  SelectStmt parse_select();
  InsertStmt parse_insert();
  UpdateStmt parse_update();
  DeleteStmt parse_delete();
  CreateTableStmt parse_create_table();
  CreateIndexStmt parse_create_index();
  DropTableStmt parse_drop_table();
  DropIndexStmt parse_drop_index();
  SetStmt parse_set();
  DescribeStmt parse_describe();

  ExprPtr parse_expr();
  ExprPtr parse_add();
  ExprPtr parse_mul();
  ExprPtr parse_unary();
  ExprPtr parse_primary();
  CondPtr parse_condition();
  CondPtr parse_or_condition();
  CondPtr parse_and_condition();
  CondPtr parse_not_condition();
  CondPtr parse_condition_atom();
  bool is_clause_keyword(const Token &t) const;
  bool is_condition_end(const Token &t) const;

  ExprPtr parse_value_expr();  // stops before clause keywords
};

// Utility used by the executor: parse a vector literal such as "[1, 2, 3]".
std::vector<double> parse_vector_text(std::string_view s, bool &ok);

}  // namespace mvdb
