// 语句执行：DDL/DML/查询/事务，行数据操作与回滚逻辑。
#include "simdb.h"

namespace mv {

// ---------------------------------------------------------------- Schema
void Schema::finalize() {
  nnum = nstr = 0;
  dim = 0;
  vec = -1;
  num_idx.assign(cols.size(), -1);
  str_idx.assign(cols.size(), -1);
  for (size_t i = 0; i < cols.size(); i++) {
    if (cols[i].ty == Ty::BIGINT) num_idx[i] = nnum++;
    else if (cols[i].ty == Ty::VARCHAR) str_idx[i] = nstr++;
    else { vec = (int)i; dim = cols[i].n; }
  }
}

int Schema::find_col(const std::string& n) const {
  for (size_t i = 0; i < cols.size(); i++) {
    if (cols[i].name.size() == n.size()) {
      bool eq = true;
      for (size_t k = 0; k < n.size(); k++) {
        char a = cols[i].name[k], b = n[k];
        if (a >= 'A' && a <= 'Z') a = char(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = char(b - 'A' + 'a');
        if (a != b) { eq = false; break; }
      }
      if (eq) return (int)i;
    }
  }
  return -1;
}

// ---------------------------------------------------------------- Table
void Table::reserve_slots(uint32_t n) {
  if (n <= pk.size()) return;
  uint32_t cap = (uint32_t)pk.size();
  if (cap < 16) cap = 16;
  while (cap < n) cap *= 2;
  pk.resize(cap);
  used.resize(cap);
  nums.resize((size_t)cap * sch.nnum);
  strs.resize((size_t)cap * sch.nstr);
  vecs.resize((size_t)cap * sch.dim);
}

uint32_t Table::alloc_slot() {
  if (!freelist.empty()) {
    uint32_t s = freelist.back();
    freelist.pop_back();
    used[s] = 1;
    return s;
  }
  uint32_t s = nslots;
  reserve_slots(s + 1);
  nslots = s + 1;
  used[s] = 1;
  return s;
}

uint32_t Table::dict_intern(std::string_view s) {
  std::string key(s);
  auto it = dictmap.find(key);
  if (it != dictmap.end()) return it->second;
  uint32_t code = (uint32_t)dict.size();
  dict.push_back(std::move(key));
  dictmap.emplace(dict.back(), code);
  return code;
}

void Table::reset() {
  nslots = live = 0;
  pk.clear();
  used.clear();
  nums.clear();
  strs.clear();
  vecs.clear();
  freelist.clear();
  pkmap.clear();
  dict.clear();
  dictmap.clear();
}

Value col_value(const Table& t, uint32_t slot, int col) {
  const Schema& s = t.sch;
  Value v;
  v.ty = s.cols[col].ty;
  if (v.ty == Ty::BIGINT) {
    v.i = t.nums[(size_t)slot * s.nnum + s.num_idx[col]];
  } else if (v.ty == Ty::VARCHAR) {
    uint32_t code = t.strs[(size_t)slot * s.nstr + s.str_idx[col]];
    v.s = t.dict_get(code);
  } else {
    v.v = &t.vecs[(size_t)slot * s.dim];
  }
  return v;
}

// ---------------------------------------------------------------- 工具
int utf8_codepoints(std::string_view s) {
  int n = 0;
  for (unsigned char c : s) {
    if ((c & 0xC0) != 0x80) n++;
  }
  return n;
}

bool parse_vec_literal(std::string_view lit, int dim, std::vector<float>& out, DbError& err) {
  const char* p = lit.data();
  const char* e = p + lit.size();
  while (p < e && (unsigned char)*p <= ' ') p++;
  if (p >= e || *p != '[') {
    err = AE(E_BAD_VALUE, "invalid vector literal: expect '[...]'");
    return false;
  }
  p++;
  out.clear();
  out.reserve((size_t)dim);
  while (true) {
    while (p < e && ((unsigned char)*p <= ' ' || *p == ',')) p++;
    if (p >= e) { err = AE(E_BAD_VALUE, "invalid vector literal: missing ']'"); return false; }
    if (*p == ']') { p++; break; }
    char* endp = nullptr;
    errno = 0;
    double d = strtod(p, &endp);
    if (endp == p) { err = AE(E_BAD_VALUE, "invalid vector literal: bad number"); return false; }
    if (!std::isfinite(d) || fabs(d) > 3.0e38) {
      err = AE(E_BAD_VALUE, "vector element out of range");
      return false;
    }
    if ((int)out.size() >= dim) {
      err = AE(E_BAD_VALUE, "vector dimension mismatch");
      return false;
    }
    out.push_back((float)d);
    p = endp;
  }
  while (p < e && (unsigned char)*p <= ' ') p++;
  if (p != e) { err = AE(E_BAD_VALUE, "invalid vector literal: trailing characters"); return false; }
  if ((int)out.size() != dim) {
    char buf[128];
    snprintf(buf, sizeof buf, "vector dimension mismatch: expect %d, got %d", dim, (int)out.size());
    err = AE(E_BAD_VALUE, buf);
    return false;
  }
  return true;
}

// ---------------------------------------------------------------- 表达式绑定/求值
static bool bind_expr(Expr& e, const Schema& s, DbError& err) {
  switch (e.k) {
    case Expr::COL: {
      e.col = s.find_col(e.sval);
      if (e.col < 0) {
        err = AE(E_BAD_COL, "Unknown column '" + e.sval + "'");
        return false;
      }
      return true;
    }
    case Expr::INT:
    case Expr::STR:
      return true;
    case Expr::CMP: {
      if (!bind_expr(*e.a, s, err)) return false;
      if (!bind_expr(*e.b, s, err)) return false;
      for (Expr* x : {e.a.get(), e.b.get()}) {
        if (x->k == Expr::COL && s.cols[x->col].ty == Ty::VECTOR) {
          err = AE(E_UNSUPPORTED, "vector column cannot be compared in WHERE");
          return false;
        }
      }
      return true;
    }
    case Expr::AND: {
      if (!bind_expr(*e.a, s, err)) return false;
      return bind_expr(*e.b, s, err);
    }
    default:
      err = AE(E_UNSUPPORTED, "unsupported expression");
      return false;
  }
}

static bool eval_expr(const Table& t, uint32_t slot, const Expr& e, Value& v, DbError& err) {
  switch (e.k) {
    case Expr::COL:
      v = col_value(t, slot, e.col);
      return true;
    case Expr::INT:
      v.ty = Ty::BIGINT;
      v.i = e.ival;
      return true;
    case Expr::STR:
      v.ty = Ty::VARCHAR;
      v.s = e.sval;
      return true;
    case Expr::CMP: {
      Value a, b;
      if (!eval_expr(t, slot, *e.a, a, err)) return false;
      if (!eval_expr(t, slot, *e.b, b, err)) return false;
      int c;
      if (a.ty == Ty::BIGINT && b.ty == Ty::BIGINT) {
        c = (a.i < b.i) ? -1 : (a.i > b.i ? 1 : 0);
      } else if (a.ty == Ty::VARCHAR && b.ty == Ty::VARCHAR) {
        int r = a.s.compare(b.s);
        c = (r < 0) ? -1 : (r > 0 ? 1 : 0);
      } else {
        err = AE(E_BAD_VALUE, "type mismatch in comparison");
        return false;
      }
      bool r = false;
      switch (e.op) {
        case 0: r = c == 0; break;
        case 1: r = c != 0; break;
        case 2: r = c < 0; break;
        case 3: r = c <= 0; break;
        case 4: r = c > 0; break;
        case 5: r = c >= 0; break;
      }
      v.ty = Ty::BIGINT;
      v.i = r ? 1 : 0;
      return true;
    }
    case Expr::AND: {
      Value a, b;
      if (!eval_expr(t, slot, *e.a, a, err)) return false;
      if (!eval_expr(t, slot, *e.b, b, err)) return false;
      v.ty = Ty::BIGINT;
      v.i = (a.ty == Ty::BIGINT && a.i != 0 && b.i != 0) ? 1 : 0;
      return true;
    }
    default:
      err = AE(E_UNSUPPORTED, "unsupported expression");
      return false;
  }
}


// ---------------------------------------------------------------- 行镜像
static void snapshot(const Table& t, uint32_t slot, RowImage& img) {
  const Schema& s = t.sch;
  img.present = t.used[slot] != 0;
  img.pk = t.pk[slot];
  img.nums.assign(t.nums.begin() + (size_t)slot * s.nnum, t.nums.begin() + (size_t)(slot + 1) * s.nnum);
  img.strs.assign(t.strs.begin() + (size_t)slot * s.nstr, t.strs.begin() + (size_t)(slot + 1) * s.nstr);
  img.vecs.assign(t.vecs.begin() + (size_t)slot * s.dim, t.vecs.begin() + (size_t)(slot + 1) * s.dim);
}

static void put_row(Table& t, uint32_t slot, const RowImage& img) {
  const Schema& s = t.sch;
  t.pk[slot] = img.pk;
  if (s.nnum) memcpy(&t.nums[(size_t)slot * s.nnum], img.nums.data(), sizeof(int64_t) * s.nnum);
  if (s.nstr) memcpy(&t.strs[(size_t)slot * s.nstr], img.strs.data(), sizeof(uint32_t) * s.nstr);
  if (s.dim) memcpy(&t.vecs[(size_t)slot * s.dim], img.vecs.data(), sizeof(float) * s.dim);
}

// ---------------------------------------------------------------- 事务
static void apply_undo(Engine& e, const UndoEntry& u) {
  Table* t = (u.tid < e.tables.size()) ? e.tables[u.tid].get() : nullptr;
  if (!t) return;
  uint32_t slot = u.slot;
  if (u.type == 0) {                     // 新插入的行：删掉
    if (t->used[slot]) {
      t->used[slot] = 0;
      t->live--;
      t->pkmap.erase(t->pk[slot]);
      t->freelist.push_back(slot);
    }
  } else {                               // 1=更新 2=删除：恢复旧行
    if (t->used[slot]) t->pkmap.erase(t->pk[slot]);
    put_row(*t, slot, u.img);
    if (!t->used[slot]) {
      t->used[slot] = 1;
      t->live++;
    }
    t->pkmap[u.img.pk] = slot;
  }
}

static void undo_to(Engine& e, size_t mark) {
  while (e.txn.undo.size() > mark) {
    apply_undo(e, e.txn.undo.back());
    e.txn.undo.pop_back();
  }
}

void Engine::rollback_txn() {
  undo_to(*this, 0);
  txn.undo.clear();
  txn.ops.clear();
  txn.active = false;
}

void txn_rollback(Engine& e) { e.rollback_txn(); }

bool txn_commit(Engine& e, DbError& err) {
  if (!e.txn.active && e.txn.ops.empty()) return true;
  return e.commit(err);
}

// ---------------------------------------------------------------- 语句级辅助
static bool implicit_commit(Engine& e, DbError& err) {
  if (e.txn.active || !e.txn.ops.empty()) return e.commit(err);
  return true;
}

static void add_op(Engine& e, uint8_t type, const std::string& tname, const RowImage& row) {
  WalOp op;
  op.type = type;
  op.tname = tname;
  op.row = row;
  e.txn.ops.push_back(std::move(op));
}

static void add_undo(Engine& e, uint8_t type, uint32_t tid, uint32_t slot, const RowImage& img) {
  UndoEntry u;
  u.type = type;
  u.tid = tid;
  u.slot = slot;
  u.img = img;
  e.txn.undo.push_back(std::move(u));
}

// 把字面量表达式转成列值写入 img
static bool literal_into(Table& t, int col, const Expr& expr, RowImage& img, DbError& err) {
  const Schema& s = t.sch;
  Ty want = s.cols[col].ty;
  if (want == Ty::BIGINT) {
    if (expr.k != Expr::INT) {
      err = AE(E_BAD_VALUE, "Incorrect integer value for column '" + s.cols[col].name + "'");
      return false;
    }
    img.nums[s.num_idx[col]] = expr.ival;
    return true;
  }
  if (want == Ty::VARCHAR) {
    if (expr.k != Expr::STR) {
      err = AE(E_BAD_VALUE, "Incorrect string value for column '" + s.cols[col].name + "'");
      return false;
    }
    int cp = utf8_codepoints(expr.sval);
    if (cp > s.cols[col].n) {
      char buf[160];
      snprintf(buf, sizeof buf, "Data too long for column '%s' (limit %d characters, got %d)",
               s.cols[col].name.c_str(), s.cols[col].n, cp);
      err = AE(E_TOO_LONG, buf);
      return false;
    }
    img.strs[s.str_idx[col]] = t.dict_intern(expr.sval);
    return true;
  }
  // VECTOR
  if (expr.k != Expr::STR) {
    err = AE(E_BAD_VALUE, "Incorrect vector value for column '" + s.cols[col].name + "'");
    return false;
  }
  std::vector<float> v;
  if (!parse_vec_literal(expr.sval, s.dim, v, err)) return false;
  memcpy(img.vecs.data(), v.data(), sizeof(float) * s.dim);
  return true;
}

static void zero_image(const Schema& s, RowImage& img) {
  img.present = true;
  img.pk = 0;
  img.nums.assign((size_t)s.nnum, 0);
  img.strs.assign((size_t)s.nstr, 0);
  img.vecs.assign((size_t)s.dim, 0.f);
}

// ---------------------------------------------------------------- SELECT 辅助
struct Emit {
  const Table* t = nullptr;
  std::vector<int> pcol;        // 投影列（-1 表示距离）
  std::vector<int> qvec_col;    // 距离项的向量列
  std::vector<std::vector<float>> qvec;
};

static std::string fmt_i64(int64_t v) {
  char buf[32];
  snprintf(buf, sizeof buf, "%lld", (long long)v);
  return std::string(buf);
}


static std::string fmt_vec(const Table& t, uint32_t slot, int col) {
  const Schema& s = t.sch;
  const float* p = &t.vecs[(size_t)slot * s.dim];
  std::string r = "[";
  for (int i = 0; i < s.dim; i++) {
    if (i) r.push_back(',');
    r += Proto::fmt_float((double)p[i]);
  }
  r.push_back(']');
  return r;
}

static double exact_l2(const Table& t, uint32_t slot, int vec_col, const std::vector<float>& q) {
  const Schema& s = t.sch;
  const float* p = &t.vecs[(size_t)slot * s.dim];
  double sum = 0.0;
  for (int i = 0; i < s.dim; i++) {
    double d = (double)p[i] - (double)q[i];
    sum += d * d;
  }
  return sqrt(sum);
}

// ---------------------------------------------------------------- 执行入口
bool exec_stmt(Engine& e, const Stmt& s, ResultSet& out, DbError& err) {
  out = ResultSet();
  switch (s.k) {
    case Stmt::EMPTY:
    case Stmt::SETVAR:
      return true;
    case Stmt::EXIT:
      return true;
    case Stmt::BEGIN: {
      if (e.txn.active) {                 // MySQL 语义：再次 BEGIN 隐式提交前一个事务
        if (!e.commit(err)) return false;
      }
      e.txn.active = true;
      e.txn.undo.clear();
      e.txn.ops.clear();
      return true;
    }
    case Stmt::COMMIT: {
      if (!e.txn.active && e.txn.ops.empty()) return true;
      return e.commit(err);
    }
    case Stmt::ROLLBACK: {
      e.rollback_txn();
      return true;
    }
    case Stmt::CREATE: {
      if (!implicit_commit(e, err)) return false;
      if (e.find(s.table)) {
        if (s.if_not_exists) return true;
        err = AE(E_TABLE_EXISTS, "Table '" + s.table + "' already exists");
        return false;
      }
      Schema sch;
      sch.name = s.table;
      sch.cols = s.cols;
      // 主键解析
      int pk = -1, vec = -1;
      for (size_t i = 0; i < sch.cols.size(); i++) {
        for (size_t j = 0; j < i; j++) {
          if (sch.cols[i].name.size() == sch.cols[j].name.size()) {
            std::string a = sch.cols[i].name, b = sch.cols[j].name;
            for (char& c : a) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
            for (char& c : b) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
            if (a == b) {
              err = AE(E_DUP_COL, "Duplicate column name '" + sch.cols[i].name + "'");
              return false;
            }
          }
        }
        if (sch.cols[i].ty == Ty::VECTOR) {
          if (vec >= 0) { err = AE(E_UNSUPPORTED, "only one VECTOR column per table"); return false; }
          vec = (int)i;
        }
        if (!s.pk_name.empty()) {
          std::string a = sch.cols[i].name, b = s.pk_name;
          for (char& c : a) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
          for (char& c : b) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
          if (a == b) pk = (int)i;
        }
      }
      if (pk < 0) {
        err = AE(E_GENERIC, "table must have exactly one PRIMARY KEY");
        return false;
      }
      if (sch.cols[pk].ty != Ty::BIGINT) {
        err = AE(E_BAD_VALUE, "PRIMARY KEY must be BIGINT");
        return false;
      }
      sch.pk = pk;
      sch.vec = vec;
      sch.finalize();
      std::unique_ptr<Table> t(new Table());
      t->sch = sch;
      uint32_t tid = (uint32_t)e.tables.size();
      e.tmap[sch.name] = tid;
      e.tables.push_back(std::move(t));
      e.txn.ops.clear();
      std::string m;
      if (!e.save_base(m)) { err = AE(E_GENERIC, "persist schema failed: " + m); return false; }
      return true;
    }
    case Stmt::DROP: {
      if (!implicit_commit(e, err)) return false;
      auto it = e.tmap.find(s.table);
      if (it == e.tmap.end() || !e.tables[it->second] || e.tables[it->second]->dropped) {
        if (s.if_exists) return true;
        err = AE(E_NO_TABLE, "Unknown table '" + s.table + "'");
        return false;
      }
      e.tables[it->second]->reset();
      e.tables[it->second]->dropped = true;
      e.tmap.erase(it);
      std::string m;
      if (!e.save_base(m)) { err = AE(E_GENERIC, "persist schema failed: " + m); return false; }
      return true;
    }
    case Stmt::TRUNCATE: {
      Table* t = e.find(s.table);
      if (!t) { err = AE(E_NO_TABLE, "Unknown table '" + s.table + "'"); return false; }
      RowImage img;
      zero_image(t->sch, img);
      size_t n = 0;
      for (uint32_t slot = 0; slot < t->nslots; slot++) {
        if (!t->used[slot]) continue;
        snapshot(*t, slot, img);
        add_op(e, 3, t->sch.name, img);
        n++;
      }
      t->reset();
      out.affected = (int64_t)n;
      if (!e.txn.active) return e.commit(err);
      return true;
    }
    case Stmt::VINDEX: {
      if (!implicit_commit(e, err)) return false;
      Table* t = e.find(s.table);
      if (!t) { err = AE(E_NO_TABLE, "Unknown table '" + s.table + "'"); return false; }
      int col = t->sch.find_col(s.cols.empty() ? std::string() : s.cols[0].name);
      if (col < 0 || t->sch.cols[col].ty != Ty::VECTOR) {
        err = AE(E_BAD_COL, "vector column not found");
        return false;
      }
      e.ann_index = true;
      return true;
    }
    case Stmt::SHOW_TABLES: {
      std::vector<std::string> names;
      for (auto& tp : e.tables) if (tp && !tp->dropped) names.push_back(tp->sch.name);
      std::sort(names.begin(), names.end());
      out.has_rows = true;
      out.cols = 1;
      out.headers.push_back("Tables_in_minivectordb");
      for (auto& n : names) out.cells.push_back(n);
      return true;
    }
    case Stmt::DESC: {
      Table* t = e.find(s.table);
      if (!t) { err = AE(E_NO_TABLE, "Unknown table '" + s.table + "'"); return false; }
      out.has_rows = true;
      out.cols = 6;
      out.headers = {"Field", "Type", "Null", "Key", "Default", "Extra"};
      for (size_t i = 0; i < t->sch.cols.size(); i++) {
        const ColDef& c = t->sch.cols[i];
        std::string ty = ty_name(c.ty);
        if (c.ty == Ty::VARCHAR) ty += "(" + std::to_string(c.n) + ")";
        else if (c.ty == Ty::VECTOR) ty += "(" + std::to_string(c.n) + ")";
        out.cells.push_back(c.name);
        out.cells.push_back(ty);
        out.cells.push_back("YES");
        out.cells.push_back((int)i == t->sch.pk ? "PRI" : "");
        out.cells.push_back("NULL");
        out.cells.push_back("");
      }
      return true;
    }
    case Stmt::INSERT: {
      Table* t = e.find(s.table);
      if (!t) { err = AE(E_NO_TABLE, "Table '" + s.table + "' doesn't exist"); return false; }
      const Schema& sch = t->sch;
      std::vector<int> colmap;
      if (s.names.empty()) {
        for (size_t i = 0; i < sch.cols.size(); i++) colmap.push_back((int)i);
      } else {
        for (auto& n : s.names) {
          int c = sch.find_col(n);
          if (c < 0) { err = AE(E_BAD_COL, "Unknown column '" + n + "' in field list"); return false; }
          for (int x : colmap) if (x == c) { err = AE(E_DUP_COL, "Column '" + n + "' specified twice"); return false; }
          colmap.push_back(c);
        }
      }
      size_t mark = e.txn.undo.size();
      size_t omark = e.txn.ops.size();
      std::vector<int64_t> seen;
      seen.reserve(s.rows.size());
      for (size_t ri = 0; ri < s.rows.size(); ri++) {
        const auto& row = s.rows[ri];
        if (row.size() != colmap.size()) {
          char buf[128];
          snprintf(buf, sizeof buf, "Column count doesn't match value count at row %u", (unsigned)(ri + 1));
          err = AE(E_BAD_COUNT, buf);
          undo_to(e, mark);
          e.txn.ops.resize(omark);
          return false;
        }
        RowImage img;
        zero_image(sch, img);
        for (size_t i = 0; i < row.size(); i++) {
          if (row[i]->k != Expr::INT && row[i]->k != Expr::STR) {
            err = AE(E_BAD_VALUE, "expressions are not supported in VALUES");
            undo_to(e, mark);
            e.txn.ops.resize(omark);
            return false;
          }
          if (!literal_into(*t, colmap[i], *row[i], img, err)) {
            undo_to(e, mark);
            e.txn.ops.resize(omark);
            return false;
          }
        }
        img.pk = img.nums[sch.num_idx[sch.pk]];
        if (t->pkmap.count(img.pk)) {
          err = AE(E_DUP_KEY, "Duplicate entry '" + fmt_i64(img.pk) + "' for key 'PRIMARY'");
          undo_to(e, mark);
          e.txn.ops.resize(omark);
          return false;
        }
        for (int64_t v : seen) {
          if (v == img.pk) {
            err = AE(E_DUP_KEY, "Duplicate entry '" + fmt_i64(img.pk) + "' for key 'PRIMARY'");
            undo_to(e, mark);
            e.txn.ops.resize(omark);
            return false;
          }
        }
        seen.push_back(img.pk);
        uint32_t slot = t->alloc_slot();
        put_row(*t, slot, img);
        t->live++;
        t->pkmap[img.pk] = slot;
        add_undo(e, 0, (uint32_t)(e.tmap[sch.name]), slot, RowImage());
        add_op(e, 1, sch.name, img);
      }
      out.affected = (long long)s.rows.size();
      if (!e.txn.active) return e.commit(err);
      return true;
    }
    case Stmt::UPDATE: {
      Table* t = e.find(s.table);
      if (!t) { err = AE(E_NO_TABLE, "Table '" + s.table + "' doesn't exist"); return false; }
      const Schema& sch = t->sch;
      std::vector<int> cols;
      for (auto& n : s.names) {
        int c = sch.find_col(n);
        if (c < 0) { err = AE(E_BAD_COL, "Unknown column '" + n + "' in 'field list'"); return false; }
        cols.push_back(c);
      }
      if (s.where && !bind_expr(*s.where, sch, err)) return false;
      for (auto& st : s.sets) {
        if (!bind_expr(*st, sch, err)) return false;
      }
      size_t mark = e.txn.undo.size();
      size_t omark = e.txn.ops.size();
      uint32_t tid = e.tmap[sch.name];
      long long n = 0;
      for (uint32_t slot = 0; slot < t->nslots; slot++) {
        if (!t->used[slot]) continue;
        if (s.where) {
          Value v;
          if (!eval_expr(*t, slot, *s.where, v, err)) { undo_to(e, mark); e.txn.ops.resize(omark); return false; }
          if (!(v.ty == Ty::BIGINT && v.i != 0)) continue;
        }
        RowImage before;
        snapshot(*t, slot, before);
        RowImage after = before;
        for (size_t i = 0; i < s.sets.size(); i++) {
          Value v;
          if (!eval_expr(*t, slot, *s.sets[i], v, err)) { undo_to(e, mark); e.txn.ops.resize(omark); return false; }
          Expr tmp;
          if (v.ty == Ty::BIGINT) tmp.k = Expr::INT, tmp.ival = v.i;
          else if (v.ty == Ty::VARCHAR) tmp.k = Expr::STR, tmp.sval = std::string(v.s);
          else { err = AE(E_BAD_VALUE, "cannot assign vector value"); undo_to(e, mark); e.txn.ops.resize(omark); return false; }
          if (!literal_into(*t, cols[i], tmp, after, err)) { undo_to(e, mark); e.txn.ops.resize(omark); return false; }
        }
        after.pk = after.nums[sch.num_idx[sch.pk]];   // 主键列可能被 SET 改写
        if (after.pk != before.pk) {
          auto f = t->pkmap.find(after.pk);
          if (f != t->pkmap.end() && f->second != slot) {
            char buf[128];
            snprintf(buf, sizeof buf, "Duplicate entry '%lld' for key 'PRIMARY'", (long long)after.pk);
            undo_to(e, mark);
            e.txn.ops.resize(omark);
            err = AE(E_DUP_KEY, buf);
            return false;
          }
          t->pkmap.erase(before.pk);
          t->pkmap[after.pk] = slot;
        }
        add_undo(e, 1, tid, slot, before);
        put_row(*t, slot, after);
        if (after.pk != before.pk) {
          // 主键变更：WAL 里必须拆成「删旧主键 + 插新行」，否则重放会留下旧行
          RowImage del;
          zero_image(sch, del);
          del.pk = before.pk;
          add_op(e, 3, sch.name, del);
          add_op(e, 1, sch.name, after);
        } else {
          add_op(e, 2, sch.name, after);
        }
        n++;
      }
      out.affected = n;
      if (!e.txn.active) return e.commit(err);
      return true;
    }
    case Stmt::DELETE_: {
      Table* t = e.find(s.table);
      if (!t) { err = AE(E_NO_TABLE, "Table '" + s.table + "' doesn't exist"); return false; }
      if (s.where && !bind_expr(*s.where, t->sch, err)) return false;
      size_t mark = e.txn.undo.size();
      size_t omark = e.txn.ops.size();
      uint32_t tid = e.tmap[t->sch.name];
      long long n = 0;
      for (uint32_t slot = 0; slot < t->nslots; slot++) {
        if (!t->used[slot]) continue;
        if (s.where) {
          Value v;
          if (!eval_expr(*t, slot, *s.where, v, err)) { undo_to(e, mark); e.txn.ops.resize(omark); return false; }
          if (!(v.ty == Ty::BIGINT && v.i != 0)) continue;
        }
        RowImage before;
        snapshot(*t, slot, before);
        add_undo(e, 2, tid, slot, before);
        t->used[slot] = 0;
        t->live--;
        t->pkmap.erase(t->pk[slot]);
        t->freelist.push_back(slot);
        add_op(e, 3, t->sch.name, before);
        n++;
      }
      out.affected = n;
      if (!e.txn.active) return e.commit(err);
      return true;
    }
    case Stmt::SELECT: {
      if (s.no_table) {
        out.has_rows = true;
        for (auto& it : s.items) {
          if (it.agg) { err = AE(E_UNSUPPORTED, "COUNT(*) requires a table"); return false; }
          if (!it.lit_int && it.lit.empty()) {
            err = AE(E_BAD_COL, "Unknown column '" + it.col_name + "'");
            return false;
          }
          out.headers.push_back(it.alias.empty() ? it.lit : it.alias);
          out.cells.push_back(it.lit);
        }
        out.cols = out.headers.size();
        return true;
      }
      Table* t = e.find(s.table);
      if (!t) { err = AE(E_NO_TABLE, "Table '" + s.table + "' doesn't exist"); return false; }
      const Schema& sch = t->sch;
      if (s.where && !bind_expr(*s.where, sch, err)) return false;
      for (auto& it : s.items) {
        if (!it.agg) continue;
        int64_t n = 0;
        for (uint32_t slot = 0; slot < t->nslots; slot++) {
          if (!t->used[slot]) continue;
          if (s.where) {
            Value v;
            if (!eval_expr(*t, slot, *s.where, v, err)) return false;
            if (!(v.ty == Ty::BIGINT && v.i != 0)) continue;
          }
          n++;
        }
        out.has_rows = true;
        out.cols = 1;
        out.headers.push_back(it.alias.empty() ? "COUNT(*)" : it.alias);
        out.cells.push_back(fmt_i64(n));
        return true;
      }

      // 投影
      std::vector<int> pcol;
      std::vector<std::vector<float>> qvec;
      std::vector<int> qcol;
      std::vector<std::string> headers;
      if (s.star) {
        for (size_t i = 0; i < sch.cols.size(); i++) {
          pcol.push_back((int)i);
          qcol.push_back(-1);
          qvec.push_back(std::vector<float>());
          headers.push_back(sch.cols[i].name);
        }
      } else {
        for (auto& it : s.items) {
          if (it.dist) {
            int c = sch.find_col(it.col_name);
            if (c < 0) { err = AE(E_BAD_COL, "Unknown column '" + it.col_name + "'"); return false; }
            if (sch.cols[c].ty != Ty::VECTOR) { err = AE(E_BAD_VALUE, "L2_DISTANCE requires a VECTOR column"); return false; }
            std::vector<float> q;
            if (!parse_vec_literal(it.vec_lit, sch.dim, q, err)) return false;
            pcol.push_back(-1);
            qcol.push_back(c);
            qvec.push_back(std::move(q));
            headers.push_back(it.alias.empty() ? ("L2_DISTANCE(" + it.col_name + ")") : it.alias);
          } else {
            int c = sch.find_col(it.col_name);
            if (c < 0) { err = AE(E_BAD_COL, "Unknown column '" + it.col_name + "'"); return false; }
            pcol.push_back(c);
            qcol.push_back(-1);
            qvec.push_back(std::vector<float>());
            headers.push_back(it.alias.empty() ? sch.cols[c].name : it.alias);
          }
        }
      }
      if (pcol.empty() && !s.star) { err = AE(E_SYNTAX, "empty select list"); return false; }

      // ORDER BY 解析
      struct SortKey {
        int proj = -1;     // 投影序号（距离/列都在投影里）
        int col = -1;      // 非投影列
        bool dist = false;
        bool desc = false;
        int qidx = -1;
      };
      std::vector<SortKey> keys;
      for (auto& ok : s.order) {
        SortKey k;
        k.desc = ok.desc;
        bool found = false;
        // 序号
        if (!ok.name.empty() && ok.name[0] >= '0' && ok.name[0] <= '9') {
          long long ord = atoll(ok.name.c_str());
          if (ord >= 1 && (size_t)ord <= pcol.size()) { k.proj = (int)ord - 1; found = true; }
        }
        if (!found) {
          for (size_t i = 0; i < pcol.size(); i++) {
            const std::string& want = ok.name;
            if (pcol[i] >= 0) {
              const std::string& cn = sch.cols[pcol[i]].name;
              if (cn.size() == want.size()) {
                bool eq = true;
                for (size_t z = 0; z < want.size(); z++) {
                  char a = cn[z], b = want[z];
                  if (a >= 'A' && a <= 'Z') a = char(a - 'A' + 'a');
                  if (b >= 'A' && b <= 'Z') b = char(b - 'A' + 'a');
                  if (a != b) { eq = false; break; }
                }
                if (eq) { k.proj = (int)i; found = true; break; }
              }
            }
            // 别名匹配
            if (i < s.items.size()) {
              const std::string& al = s.items[i].alias;
              if (!al.empty() && al.size() == want.size()) {
                bool eq = true;
                for (size_t z = 0; z < want.size(); z++) {
                  char a = al[z], b = want[z];
                  if (a >= 'A' && a <= 'Z') a = char(a - 'A' + 'a');
                  if (b >= 'A' && b <= 'Z') b = char(b - 'A' + 'a');
                  if (a != b) { eq = false; break; }
                }
                if (eq) { k.proj = (int)i; found = true; break; }
              }
            }
          }
        }
        if (!found) {
          int c = sch.find_col(ok.name);
          if (c >= 0) { k.col = c; found = true; }
        }
        if (!found) { err = AE(E_BAD_COL, "Unknown column '" + ok.name + "' in 'order clause'"); return false; }
        if (k.proj >= 0 && pcol[k.proj] < 0) { k.dist = true; k.qidx = k.proj; }
        keys.push_back(k);
      }

      long long limit = s.limit < 0 ? -1 : s.limit;
      out.has_rows = true;
      out.cols = pcol.size();
      out.headers = headers;

      // 快速路径：单键 = 距离升序 + LIMIT
      long long need = limit;
      if (need >= 0 && s.offset > 0) need += s.offset;
      bool fast = keys.size() == 1 && keys[0].dist && !keys[0].desc && limit >= 0;
      if (fast) {
        int qidx = keys[0].qidx;
        const std::vector<float>& q = qvec[qidx];
        std::vector<uint8_t> mask;
        if (s.where) {
          mask.assign(t->nslots, 0);
          for (uint32_t slot = 0; slot < t->nslots; slot++) {
            if (!t->used[slot]) continue;
            Value v;
            if (!eval_expr(*t, slot, *s.where, v, err)) return false;
            if (v.ty == Ty::BIGINT && v.i != 0) mask[slot] = 1;
          }
        }
        std::vector<Hit> hits((size_t)need + 1);
        size_t n = vec_topk(*t, q.data(), mask.empty() ? nullptr : mask.data(), (size_t)need, hits.data());
        // 用 double 精确复算距离并按 (距离, 主键) 排序
        std::vector<std::pair<double, uint32_t>> order2;
        order2.reserve(n);
        int vcol = qcol[qidx];
        for (size_t i = 0; i < n; i++) {
          double d = exact_l2(*t, hits[i].slot, vcol, q);
          order2.emplace_back(d, hits[i].slot);
        }
        std::sort(order2.begin(), order2.end(), [&](const std::pair<double, uint32_t>& a,
                                                    const std::pair<double, uint32_t>& b) {
          if (a.first != b.first) return a.first < b.first;
          return t->pk[a.second] < t->pk[b.second];
        });
        size_t skip = (size_t)(s.offset > 0 ? s.offset : 0);
        size_t emitted = 0;
        for (auto& r : order2) {
          if (skip > 0) { skip--; continue; }
          if (limit >= 0 && emitted >= (size_t)limit) break;
          emitted++;
          uint32_t slot = r.second;
          for (size_t i = 0; i < pcol.size(); i++) {
            if (pcol[i] >= 0) {
              Value v = col_value(*t, slot, pcol[i]);
              if (v.ty == Ty::BIGINT) out.cells.push_back(fmt_i64(v.i));
              else if (v.ty == Ty::VARCHAR) out.cells.push_back(std::string(v.s));
              else out.cells.push_back(fmt_vec(*t, slot, pcol[i]));
            } else {
              out.cells.push_back(Proto::fmt_float(r.first));
            }
          }
        }
        return true;
      }

      // 通用路径
      struct Cand {
        uint32_t slot;
        std::vector<Value> kcol;
        std::vector<double> kdist;
        std::vector<int64_t> knum;
      };
      std::vector<Cand> cands;
      cands.reserve(t->live);
      for (uint32_t slot = 0; slot < t->nslots; slot++) {
        if (!t->used[slot]) continue;
        if (s.where) {
          Value v;
          if (!eval_expr(*t, slot, *s.where, v, err)) return false;
          if (!(v.ty == Ty::BIGINT && v.i != 0)) continue;
        }
        Cand c;
        c.slot = slot;
        for (auto& k : keys) {
          c.knum.push_back(0);
          c.kcol.push_back(Value());
          c.kdist.push_back(0.0);
          if (k.dist) {
            const std::vector<float>& q = qvec[k.qidx];
            c.kdist.back() = exact_l2(*t, slot, qcol[k.qidx], q);
          } else if (k.proj >= 0) {
            c.kcol.back() = col_value(*t, slot, pcol[k.proj]);
          } else {
            c.kcol.back() = col_value(*t, slot, k.col);
          }
        }
        cands.push_back(std::move(c));
      }
      if (!keys.empty()) {
        // 并列最终按主键升序
        std::stable_sort(cands.begin(), cands.end(), [&](const Cand& a, const Cand& b) {
          for (size_t i = 0; i < keys.size(); i++) {
            int c = 0;
            if (keys[i].dist) {
              if (a.kdist[i] < b.kdist[i]) c = -1;
              else if (a.kdist[i] > b.kdist[i]) c = 1;
            } else {
              const Value& x = a.kcol[i];
              const Value& y = b.kcol[i];
              if (x.ty == Ty::BIGINT && y.ty == Ty::BIGINT) c = (x.i < y.i) ? -1 : (x.i > y.i ? 1 : 0);
              else c = x.s.compare(y.s);
              if (c < 0) c = -1;
              else if (c > 0) c = 1;
            }
            if (c != 0) return keys[i].desc ? c > 0 : c < 0;
          }
          return t->pk[a.slot] < t->pk[b.slot];
        });
      }
      size_t start = (size_t)(s.offset > 0 ? s.offset : 0);
      size_t nrow = cands.size();
      if (start > nrow) start = nrow;
      size_t end = nrow;
      if (limit >= 0 && start + (size_t)limit < end) end = start + (size_t)limit;
      for (size_t r = start; r < end; r++) {
        uint32_t slot = cands[r].slot;
        for (size_t i = 0; i < pcol.size(); i++) {
          if (pcol[i] >= 0) {
            Value v = col_value(*t, slot, pcol[i]);
            if (v.ty == Ty::BIGINT) out.cells.push_back(fmt_i64(v.i));
            else if (v.ty == Ty::VARCHAR) out.cells.push_back(std::string(v.s));
            else out.cells.push_back(fmt_vec(*t, slot, pcol[i]));
          } else {
            out.cells.push_back(Proto::fmt_float(exact_l2(*t, slot, qcol[i], qvec[i])));
          }
        }
      }
      return true;
    }
  }
  err = AE(E_UNSUPPORTED, "unsupported statement");
  return false;
}

}  // namespace mv
