// 手写词法 + 递归下降语法分析。
// 只覆盖题面约定的 SQL 子集；不支持的语法一律报 1064，绝不静默执行。
#include "simdb.h"

namespace mv {

namespace {

inline bool is_ident0(unsigned char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80;
}
inline bool is_ident(unsigned char c) {
  return is_ident0(c) || (c >= '0' && c <= '9') || c == '$';
}
inline bool ieq(std::string_view a, const char* b) {
  size_t n = strlen(b);
  if (a.size() != n) return false;
  for (size_t i = 0; i < n; i++) {
    char x = a[i], y = b[i];
    if (x >= 'a' && x <= 'z') x = char(x - 'a' + 'A');
    if (y >= 'a' && y <= 'z') y = char(y - 'a' + 'A');
    if (x != y) return false;
  }
  return true;
}
std::string lower(std::string_view s) {
  std::string r(s);
  for (char& c : r) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
  return r;
}

}  // namespace

// ---------------------------------------------------------------- 词法
bool Lexer::next(Token& out) {
  const std::string& s = src;
  while (pos < s.size()) {
    unsigned char c = (unsigned char)s[pos];
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { pos++; continue; }
    if (c == '-' && pos + 1 < s.size() && s[pos + 1] == '-') {
      while (pos < s.size() && s[pos] != '\n') pos++;
      continue;
    }
    if (c == '#') {
      while (pos < s.size() && s[pos] != '\n') pos++;
      continue;
    }
    if (c == '/' && pos + 1 < s.size() && s[pos + 1] == '*') {
      pos += 2;
      while (pos + 1 < s.size() && !(s[pos] == '*' && s[pos + 1] == '/')) pos++;
      pos = (pos + 1 < s.size()) ? pos + 2 : s.size();
      continue;
    }
    break;
  }
  if (pos >= s.size()) { out.t = Token::END; out.text.clear(); return false; }

  unsigned char c = (unsigned char)s[pos];
  if (c == '\'' || c == '"') {
    char q = (char)c;
    pos++;
    std::string v;
    while (pos < s.size()) {
      char ch = s[pos];
      if (ch == '\\' && pos + 1 < s.size()) {
        char e = s[pos + 1];
        switch (e) {
          case 'n': v.push_back('\n'); break;
          case 't': v.push_back('\t'); break;
          case 'r': v.push_back('\r'); break;
          case '0': v.push_back('\0'); break;
          case '\\': v.push_back('\\'); break;
          case '\'': v.push_back('\''); break;
          case '"': v.push_back('"'); break;
          default: v.push_back(e); break;
        }
        pos += 2;
        continue;
      }
      if (ch == q) {
        if (pos + 1 < s.size() && s[pos + 1] == q) { v.push_back(q); pos += 2; continue; }
        pos++;
        out.t = Token::STR;
        out.text = std::move(v);
        return true;
      }
      v.push_back(ch);
      pos++;
    }
    out.t = Token::STR;
    out.text = std::move(v);
    return true;
  }
  if (c == '`') {
    pos++;
    std::string v;
    while (pos < s.size() && s[pos] != '`') v.push_back(s[pos++]);
    if (pos < s.size()) pos++;
    out.t = Token::ID;
    out.text = std::move(v);
    return true;
  }
  if (is_ident0(c)) {
    size_t b = pos;
    while (pos < s.size() && is_ident((unsigned char)s[pos])) pos++;
    out.t = Token::ID;
    out.text = s.substr(b, pos - b);
    return true;
  }
  if (c >= '0' && c <= '9') {
    size_t b = pos;
    while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') pos++;
    out.t = Token::NUM;
    out.text = s.substr(b, pos - b);
    return true;
  }
  static const char* two[] = {"<=", ">=", "<>", "!=", nullptr};
  for (int i2 = 0; two[i2]; i2++) {
    if (s.compare(pos, 2, two[i2]) == 0) {
      out.t = Token::PUNCT;
      out.text = two[i2];
      pos += 2;
      return true;
    }
  }
  out.t = Token::PUNCT;
  out.text.assign(1, (char)c);
  pos++;
  return true;
}

// ---------------------------------------------------------------- 语法
bool Parser::fail(const std::string& m) {
  if (emsg.empty()) emsg = m;
  return false;
}
bool Parser::peek(const char* kw) {
  return i < toks.size() && toks[i].t == Token::ID && ieq(toks[i].text, kw);
}
bool Parser::accept(const char* kw) {
  if (peek(kw)) { i++; return true; }
  return false;
}
bool Parser::accept_punct(const char* p) {
  if (i < toks.size() && toks[i].t == Token::PUNCT && toks[i].text == p) { i++; return true; }
  return false;
}
bool Parser::expect_punct(const char* p) {
  if (accept_punct(p)) return true;
  return fail(std::string("expect '") + p + "'");
}
bool Parser::ident(std::string& out) {
  if (i < toks.size() && toks[i].t == Token::ID) {
    out = toks[i].text;
    i++;
    return true;
  }
  return fail("expect identifier");
}

bool Parser::vec_lit(std::string& out) {
  // 向量字面量：优先带引号字符串；同时容错不带引号的 [1,2,3]
  if (i < toks.size() && toks[i].t == Token::STR) {
    out = toks[i].text;
    i++;
    return true;
  }
  if (i < toks.size() && toks[i].t == Token::PUNCT && toks[i].text == "[") {
    i++;
    out = "[";
    bool first = true;
    while (i < toks.size()) {
      Token& x = toks[i];
      if (x.t == Token::PUNCT && x.text == "]") {
        i++;
        out += "]";
        return true;
      }
      if (x.t == Token::PUNCT && x.text == ",") {
        out += ",";
        i++;
        first = true;
        continue;
      }
      if (!first) {
        fail("invalid vector literal");
        return false;
      }
      out += x.text;
      first = false;
      i++;
    }
    fail("invalid vector literal: missing ']'");
    return false;
  }
  fail("expect vector literal");
  return false;
}

std::unique_ptr<Expr> Parser::parse_primary() {
  if (i >= toks.size()) { fail("unexpected end of statement"); return nullptr; }
  Token& t = toks[i];
  if (t.t == Token::PUNCT && t.text == "(") {
    i++;
    auto e = parse_or();
    if (!e) return nullptr;
    if (!expect_punct(")")) return nullptr;
    return e;
  }
  if (t.t == Token::PUNCT && t.text == "-") {
    i++;
    if (i < toks.size() && toks[i].t == Token::NUM) {
      auto e = std::make_unique<Expr>();
      e->k = Expr::INT;
      errno = 0;
      e->ival = -strtoll(toks[i].text.c_str(), nullptr, 10);
      if (errno == ERANGE) { fail("integer literal out of range"); return nullptr; }
      i++;
      return e;
    }
    fail("expect number after '-'");
    return nullptr;
  }
  if (t.t == Token::NUM) {
    auto e = std::make_unique<Expr>();
    e->k = Expr::INT;
    errno = 0;
    e->ival = strtoll(t.text.c_str(), nullptr, 10);
    if (errno == ERANGE) { fail("integer literal out of range"); return nullptr; }
    i++;
    return e;
  }
  if (t.t == Token::STR) {
    auto e = std::make_unique<Expr>();
    e->k = Expr::STR;
    e->sval = t.text;
    i++;
    return e;
  }
  if (t.t == Token::PUNCT && t.text == "[") {
    auto e = std::make_unique<Expr>();
    e->k = Expr::STR;
    if (!vec_lit(e->sval)) return nullptr;
    return e;
  }
  if (t.t == Token::ID) {
    if (ieq(t.text, "L2_DISTANCE")) {
      i++;
      if (!expect_punct("(")) return nullptr;
      std::string colname;
      if (!ident(colname)) return nullptr;
      if (!expect_punct(",")) return nullptr;
      auto e = std::make_unique<Expr>();
      e->k = Expr::L2;
      if (!vec_lit(e->sval)) return nullptr;
      if (!expect_punct(")")) return nullptr;
      auto cnode = std::make_unique<Expr>();
      cnode->k = Expr::COL;
      cnode->sval = colname;
      e->a = std::move(cnode);
      return e;
    }
    if (ieq(t.text, "TRUE")) { i++; auto e = std::make_unique<Expr>(); e->k = Expr::INT; e->ival = 1; return e; }
    if (ieq(t.text, "FALSE")) { i++; auto e = std::make_unique<Expr>(); e->k = Expr::INT; e->ival = 0; return e; }
    auto e = std::make_unique<Expr>();
    e->k = Expr::COL;
    e->sval = t.text;
    i++;
    return e;
  }
  fail("unexpected token '" + t.text + "'");
  return nullptr;
}

std::unique_ptr<Expr> Parser::parse_cmp() {
  auto a = parse_primary();
  if (!a) return nullptr;
  if (i >= toks.size() || toks[i].t != Token::PUNCT) return a;
  const std::string& op = toks[i].text;
  int o = -1;
  if (op == "=") o = 0;
  else if (op == "<>" || op == "!=") o = 1;
  else if (op == "<") o = 2;
  else if (op == "<=") o = 3;
  else if (op == ">") o = 4;
  else if (op == ">=") o = 5;
  if (o < 0) return a;
  i++;
  auto b = parse_primary();
  if (!b) return nullptr;
  auto e = std::make_unique<Expr>();
  e->k = Expr::CMP;
  e->op = (uint8_t)o;
  e->a = std::move(a);
  e->b = std::move(b);
  return e;
}

std::unique_ptr<Expr> Parser::parse_and() {
  auto a = parse_cmp();
  if (!a) return nullptr;
  while (peek("AND")) {
    i++;
    auto b = parse_cmp();
    if (!b) return nullptr;
    auto e = std::make_unique<Expr>();
    e->k = Expr::AND;
    e->a = std::move(a);
    e->b = std::move(b);
    a = std::move(e);
  }
  return a;
}

std::unique_ptr<Expr> Parser::parse_or() { return parse_and(); }

bool Parser::parse_create(Stmt& out) {
  if (accept("TABLE")) {
    out.k = Stmt::CREATE;
    if (accept("IF")) {
      accept("NOT");
      if (!accept("EXISTS")) return fail("expect EXISTS");
      out.if_not_exists = true;
    }
    if (!ident(out.table)) return false;
    if (!expect_punct("(")) return false;
    while (true) {
      ColDef c;
      if (!ident(c.name)) return false;
      std::string ty;
      if (!ident(ty)) return false;
      std::string lt = lower(ty);
      if (lt == "bigint" || lt == "int" || lt == "integer" || lt == "long") {
        c.ty = Ty::BIGINT;
      } else if (lt == "varchar" || lt == "char" || lt == "text") {
        c.ty = Ty::VARCHAR;
        if (accept_punct("(")) {
          if (i >= toks.size() || toks[i].t != Token::NUM) return fail("expect length");
          c.n = atoi(toks[i].text.c_str());
          i++;
          if (!expect_punct(")")) return false;
        } else {
          c.n = 256;
        }
        if (c.n <= 0) return fail("invalid varchar length");
      } else if (lt == "vector" || lt == "float_vector") {
        c.ty = Ty::VECTOR;
        if (!expect_punct("(")) return false;
        if (i >= toks.size() || toks[i].t != Token::NUM) return fail("expect dimension");
        c.n = atoi(toks[i].text.c_str());
        i++;
        if (!expect_punct(")")) return false;
        if (c.n <= 0 || c.n > 65536) return fail("invalid vector dimension");
      } else {
        return fail("unsupported column type '" + ty + "'");
      }
      bool is_pk = false;
      if (peek("PRIMARY")) { i++; accept("KEY"); is_pk = true; }
      if (is_pk) out.pk_name = c.name;
      out.cols.push_back(std::move(c));
      if (accept_punct(",")) continue;
      if (accept_punct(")")) break;
      return fail("expect ',' or ')' in column list");
    }
    if (accept_punct(",")) {          // 表级约束：PRIMARY KEY (col)
      if (!accept("PRIMARY")) return fail("expect PRIMARY");
      if (!accept("KEY")) return fail("expect KEY");
      if (!expect_punct("(")) return false;
      std::string pkcol;
      if (!ident(pkcol)) return false;
      if (!expect_punct(")")) return false;
      out.pk_name = pkcol;
      if (!expect_punct(")")) return false;
    }
    return true;
  }
  if (accept("VECTOR")) {
    if (!accept("INDEX")) return fail("expect INDEX");
    out.k = Stmt::VINDEX;
    if (!ident(out.index_name)) return false;
    if (!accept("ON")) return fail("expect ON");
    if (!ident(out.table)) return false;
    if (!expect_punct("(")) return false;
    std::string vc;
    if (!ident(vc)) return false;
    if (!expect_punct(")")) return false;
    out.cols.push_back(ColDef{vc, Ty::VECTOR, 0});
    return true;
  }
  if (accept("INDEX")) {
    out.k = Stmt::VINDEX;
    if (!ident(out.index_name)) return false;
    if (!accept("ON")) return fail("expect ON");
    if (!ident(out.table)) return false;
    if (!expect_punct("(")) return false;
    std::string vc;
    if (!ident(vc)) return false;
    if (!expect_punct(")")) return false;
    out.cols.push_back(ColDef{vc, Ty::VECTOR, 0});
    return true;
  }
  return fail("unsupported CREATE statement");
}

bool Parser::parse_insert(Stmt& out) {
  out.k = Stmt::INSERT;
  if (!accept("INTO")) return fail("expect INTO");
  if (!ident(out.table)) return false;
  if (accept_punct("(")) {
    while (true) {
      std::string c;
      if (!ident(c)) return false;
      out.names.push_back(std::move(c));
      if (accept_punct(",")) continue;
      if (!expect_punct(")")) return false;
      break;
    }
  }
  if (!accept("VALUES") && !accept("VALUE")) return fail("expect VALUES");
  while (true) {
    if (!expect_punct("(")) return false;
    std::vector<std::unique_ptr<Expr>> row;
    if (!accept_punct(")")) {
      while (true) {
        auto e = parse_or();
        if (!e) return false;
        row.push_back(std::move(e));
        if (accept_punct(",")) continue;
        if (!expect_punct(")")) return false;
        break;
      }
    }
    out.rows.push_back(std::move(row));
    if (accept_punct(",")) continue;
    break;
  }
  return true;
}

bool Parser::parse_select(Stmt& out) {
  out.k = Stmt::SELECT;
  if (accept_punct("*")) {
    out.star = true;
  } else {
    while (true) {
      ProjItem it;
      if (peek("COUNT")) {
        i++;
        if (!expect_punct("(")) return false;
        if (accept_punct("*")) it.col_name = "*";
        else if (!ident(it.col_name)) return false;
        if (!expect_punct(")")) return false;
        it.agg = 1;
      } else if (peek("L2_DISTANCE")) {
        auto e = parse_primary();
        if (!e || e->k != Expr::L2) return fail("expect L2_DISTANCE(col, '[...]')");
        it.dist = true;
        it.col_name = e->a->sval;
        it.vec_lit = e->sval;
      } else {
        auto e = parse_primary();
        if (!e) return false;
        if (e->k == Expr::COL) {
          it.col_name = e->sval;
        } else if (e->k == Expr::INT) {
          it.lit_int = 1;
          it.lit = std::to_string(e->ival);
        } else if (e->k == Expr::STR) {
          it.lit = e->sval;
        } else {
          return fail("select item must be a column, literal, COUNT(*) or L2_DISTANCE(...)");
        }
      }
      if (accept("AS")) {
        if (!ident(it.alias)) return false;
      } else if (i < toks.size() && toks[i].t == Token::ID && !ieq(toks[i].text, "FROM")) {
        it.alias = toks[i].text;
        i++;
      }
      out.items.push_back(std::move(it));
      if (accept_punct(",")) continue;
      break;
    }
  }
  if (!accept("FROM")) {
    out.no_table = true;
    if (accept("LIMIT")) {
      if (i >= toks.size() || toks[i].t != Token::NUM) return fail("expect LIMIT number");
      out.limit = strtoll(toks[i].text.c_str(), nullptr, 10);
      i++;
    }
    return true;
  }
  if (!ident(out.table)) return false;
  if (accept("WHERE")) {
    out.where = parse_or();
    if (!out.where) return false;
  }
  if (accept("ORDER")) {
    if (!accept("BY")) return fail("expect BY");
    while (true) {
      OrderKey k;
      if (i < toks.size() && (toks[i].t == Token::ID || toks[i].t == Token::NUM)) {
        k.name = toks[i].text;
        i++;
      } else {
        return fail("expect order key");
      }
      if (accept("DESC")) k.desc = true;
      else accept("ASC");
      out.order.push_back(std::move(k));
      if (accept_punct(",")) continue;
      break;
    }
  }
  if (accept("LIMIT")) {
    if (i >= toks.size() || toks[i].t != Token::NUM) return fail("expect LIMIT number");
    out.limit = strtoll(toks[i].text.c_str(), nullptr, 10);
    i++;
    if (accept_punct(",")) {          // LIMIT offset, count
      if (i >= toks.size() || toks[i].t != Token::NUM) return fail("expect LIMIT count");
      out.offset = out.limit;
      out.limit = strtoll(toks[i].text.c_str(), nullptr, 10);
      i++;
    }
  }
  return true;
}

bool Parser::parse_update(Stmt& out) {
  out.k = Stmt::UPDATE;
  if (!ident(out.table)) return false;
  if (!accept("SET")) return fail("expect SET");
  while (true) {
    std::string c;
    if (!ident(c)) return false;
    if (!expect_punct("=")) return false;
    auto e = parse_or();
    if (!e) return false;
    out.names.push_back(std::move(c));
    out.sets.push_back(std::move(e));
    if (accept_punct(",")) continue;
    break;
  }
  if (accept("WHERE")) {
    out.where = parse_or();
    if (!out.where) return false;
  }
  return true;
}

bool Parser::parse_delete(Stmt& out) {
  out.k = Stmt::DELETE_;
  if (!accept("FROM")) {
    out.no_table = true;
    if (accept("LIMIT")) {
      if (i >= toks.size() || toks[i].t != Token::NUM) return fail("expect LIMIT number");
      out.limit = strtoll(toks[i].text.c_str(), nullptr, 10);
      i++;
    }
    return true;
  }
  if (!ident(out.table)) return false;
  if (accept("WHERE")) {
    out.where = parse_or();
    if (!out.where) return false;
  }
  return true;
}

bool Parser::parse_index(Stmt&) { return fail("unreachable"); }

bool Parser::parse(const std::string& sql, Stmt& out, std::string& msg) {
  toks.clear();
  i = 0;
  emsg.clear();
  Lexer lx(sql);
  Token t;
  while (lx.next(t)) {
    toks.push_back(t);
    if (toks.size() > (1u << 20)) { msg = "statement too long"; return false; }
  }
  if (toks.empty()) { out.k = Stmt::EMPTY; return true; }

  bool ok = false;
  if (peek("CREATE")) { i++; ok = parse_create(out); }
  else if (peek("INSERT")) { i++; ok = parse_insert(out); }
  else if (peek("SELECT")) { i++; ok = parse_select(out); }
  else if (peek("UPDATE")) { i++; ok = parse_update(out); }
  else if (peek("DELETE")) { i++; ok = parse_delete(out); }
  else if (peek("BEGIN") || peek("START")) {
    i++;
    if (peek("TRANSACTION")) i++;
    out.k = Stmt::BEGIN;
    ok = true;
  } else if (peek("COMMIT")) { i++; out.k = Stmt::COMMIT; ok = true; }
  else if (peek("ROLLBACK")) { i++; out.k = Stmt::ROLLBACK; ok = true; }
  else if (peek("SHOW")) { i++; accept("TABLES"); out.k = Stmt::SHOW_TABLES; ok = true; }
  else if (peek("DESC") || peek("DESCRIBE")) {
    i++;
    out.k = Stmt::DESC;
    ok = ident(out.table);
  } else if (peek("SET")) {
    i++;
    out.k = Stmt::SETVAR;
    while (i < toks.size()) i++;
    ok = true;
  } else if (peek("EXIT") || peek("QUIT") || peek("BYE")) {
    i++;
    out.k = Stmt::EXIT;
    ok = true;
  } else if (peek("DROP")) {
    i++;
    if (!accept("TABLE")) { msg = "unsupported DROP"; return false; }
    out.k = Stmt::DROP;
    if (accept("IF")) {
      if (!accept("EXISTS")) { msg = "expect EXISTS"; return false; }
      out.if_exists = true;
    }
    ok = ident(out.table);
  } else if (peek("TRUNCATE")) {
    i++;
    accept("TABLE");
    out.k = Stmt::TRUNCATE;
    ok = ident(out.table);
  } else {
    msg = "unsupported statement '" + toks[0].text + "'";
    return false;
  }
  if (!ok) {
    msg = emsg.empty() ? std::string("syntax error") : emsg;
    return false;
  }
  if (i < toks.size()) {
    msg = "unexpected token '" + toks[i].text + "'";
    return false;
  }
  return true;
}

}  // namespace mv
