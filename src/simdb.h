// MiniVectorDB —— 单机轻量向量数据库（2026 OceanBase 大赛加赛）
// 核心类型与对外接口集中在本文件，便于各实现文件共享。
#pragma once

#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>
#include <deque>
#include <unordered_map>
#include <algorithm>
#include <memory>

namespace mv {

// ---------------------------------------------------------------- 错误
struct DbError {
  int code = 0;            // 0 表示成功；非 0 为对外错误码
  std::string msg;
  bool ok() const { return code == 0; }
};

inline DbError AOk() { return DbError{}; }
inline DbError AE(int c, std::string m) {
  DbError e;
  e.code = c;
  e.msg = std::move(m);
  return e;
}

// 对外错误码：暂按 MySQL errno 语义取值，题包给出约定码表后可整体替换。
enum ErrCode : int {
  E_SYNTAX       = 1064,  // 语法错误 / 不支持的语句
  E_NO_TABLE     = 1146,  // 表不存在
  E_TABLE_EXISTS = 1050,  // 表已存在
  E_BAD_COL      = 1054,  // 列不存在
  E_DUP_COL      = 1060,  // 列重复
  E_DUP_KEY      = 1062,  // 主键重复
  E_BAD_COUNT    = 1136,  // 值数量与列数不符
  E_BAD_VALUE    = 1366,  // 值类型/维度/取值非法
  E_OUT_OF_RANGE = 1264,  // 数值越界
  E_TOO_LONG = 1406,      // 字符串超长
  E_UNSUPPORTED  = 1235,
  E_GENERIC      = 1105,
};

const char* sqlstate_of(int code);
// 错误语义名（题包用 duplicate_key 之类的名字做断言，同时保留数字码）
const char* semantic_of(int code, const std::string& msg);

// ---------------------------------------------------------------- 类型
enum class Ty : uint8_t { BIGINT = 0, VARCHAR = 1, VECTOR = 2 };

const char* ty_name(Ty t);

struct ColDef {
  std::string name;
  Ty ty = Ty::BIGINT;
  int n = 0;               // VARCHAR(n) 的码点上限 / VECTOR(n) 的维度
};

// 表定义：每表恰好一个 BIGINT 主键、至多一个向量列。
struct Schema {
  std::string name;
  std::vector<ColDef> cols;
  int pk = -1;
  int vec = -1;
  int nnum = 0;            // BIGINT 列数
  int nstr = 0;            // VARCHAR 列数
  int dim = 0;             // 向量维度
  std::vector<int> num_idx;  // 每列在 BIGINT 段中的序号，否则 -1
  std::vector<int> str_idx;  // 每列在 VARCHAR 段中的序号，否则 -1
  void finalize();
  int find_col(const std::string& n) const;
};

// ---------------------------------------------------------------- 行数据
// 列式存储：主键/整型/字典码/向量各自连续，便于 SIMD 扫描。
struct Table {
  Schema sch;
  uint32_t nslots = 0;     // 已分配槽位数
  uint32_t live = 0;       // 存活行数
  bool dropped = false;    // DROP TABLE 后保留对象，保证 WAL 里的旧 tid 仍可解析
  std::vector<int64_t> pk;         // [nslots]
  std::vector<uint8_t> used;       // [nslots] 0=空槽 1=存活
  std::vector<int64_t> nums;       // [nslots*nnum]
  std::vector<uint32_t> strs;      // [nslots*nstr] 字典码
  std::vector<float> vecs;         // [nslots*dim]
  std::vector<uint32_t> freelist;  // 可复用槽位
  std::unordered_map<int64_t, uint32_t> pkmap;
  std::deque<std::string> dict;        // 字典：deque 保证元素地址稳定
  std::unordered_map<std::string, uint32_t> dictmap;

  uint32_t dict_intern(std::string_view s);
  const std::string& dict_get(uint32_t code) const { return dict[code]; }
  uint32_t alloc_slot();
  void reset();                          // 清空数据（保留 schema）
  void reserve_slots(uint32_t n);        // 扩容到至少 n 个槽位
};

struct Value {
  Ty ty = Ty::BIGINT;
  int64_t i = 0;
  std::string_view s;      // VARCHAR：指向字典或字面量（生命周期内稳定）
  const float* v = nullptr;
};

Value col_value(const Table& t, uint32_t slot, int col);

// ---------------------------------------------------------------- 表达式
struct Expr {
  enum K : uint8_t { COL, INT, STR, VEC, L2, CMP, AND, NEG } k = INT;
  uint8_t op = 0;          // CMP: 0 '=' 1 '<>' 2 '<' 3 '<=' 4 '>' 5 '>='
  int col = -1;            // 绑定后的列号
  int64_t ival = 0;
  std::string sval;        // STR 原文 / L2 的向量字面量
  std::vector<float> vecv; // L2 解析后的查询向量
  std::unique_ptr<Expr> a, b;
};

// ---------------------------------------------------------------- 语句
struct ProjItem {
  bool dist = false;                // true: L2_DISTANCE(col, '[...]')
  int agg = 0;                      // 1 = COUNT(*)
  std::string col_name;             // 列名（执行期解析成列号）
  std::string vec_lit;              // L2 的向量字面量
  std::string alias;                // AS 别名
  std::string lit;                  // 常量投影（SELECT 1 / SELECT 'x'）
  int lit_int = 0;                  // 1 = 常量是整数
};

struct OrderKey {
  std::string name;                 // 列名 / 投影别名 / 序号
  bool desc = false;
};

struct Stmt {
  enum K : uint8_t {
    EMPTY, CREATE, DROP, TRUNCATE, INSERT, SELECT, UPDATE, DELETE_, BEGIN, COMMIT,
    ROLLBACK, SHOW_TABLES, DESC, VINDEX, SETVAR, EXIT
  } k = EMPTY;

  std::string table;
  // CREATE
  std::vector<ColDef> cols;
  std::string pk_name;
  // INSERT / UPDATE 的列名列表
  std::vector<std::string> names;
  // INSERT
  std::vector<std::vector<std::unique_ptr<Expr>>> rows;
  // SELECT
  bool star = false;
  std::vector<ProjItem> items;
  std::unique_ptr<Expr> where;
  std::vector<OrderKey> order;
  long long limit = -1;
  long long offset = 0;
  bool if_not_exists = false;
  bool if_exists = false;
  bool no_table = false;
  // UPDATE
  std::vector<std::unique_ptr<Expr>> sets;
  // 索引
  std::string index_name;
};

// ---------------------------------------------------------------- 词法/语法
struct Token {
  enum T : uint8_t { ID, STR, NUM, PUNCT, END } t = END;
  std::string text;
};

struct Lexer {
  const std::string& src;
  size_t pos = 0;
  explicit Lexer(const std::string& s) : src(s) {}
  bool next(Token& out);
};

struct Parser {
  std::vector<Token> toks;
  size_t i = 0;
  std::string emsg;

  bool parse(const std::string& sql, Stmt& out, std::string& msg);
  bool peek(const char* kw);
  bool accept(const char* kw);
  bool accept_punct(const char* p);
  bool expect_punct(const char* p);
  bool ident(std::string& out);
  bool fail(const std::string& m);
  bool parse_create(Stmt&);
  bool parse_insert(Stmt&);
  bool parse_select(Stmt&);
  bool parse_update(Stmt&);
  bool parse_delete(Stmt&);
  bool parse_index(Stmt&);
  bool vec_lit(std::string& out);
  std::unique_ptr<Expr> parse_or();
  std::unique_ptr<Expr> parse_and();
  std::unique_ptr<Expr> parse_cmp();
  std::unique_ptr<Expr> parse_primary();
};

// ---------------------------------------------------------------- 向量内核
struct Hit {
  float d2 = 0.f;   // 平方欧氏距离
  int64_t pk = 0;
  uint32_t slot = 0;
};

size_t vec_topk(const Table& t, const float* q, const uint8_t* mask, size_t k, Hit* out);
float vec_l2sq(const float* a, const float* b, int dim);
int simd_level();   // 0 标量 / 2 avx2+fma / 3 avx512

// ---------------------------------------------------------------- 持久化
void put_u8(std::string& b, uint8_t v);
void put_u32(std::string& b, uint32_t v);
void put_u64(std::string& b, uint64_t v);
void put_i64(std::string& b, int64_t v);
void put_str(std::string& b, std::string_view s);
void put_f32(std::string& b, float v);
uint32_t crc32c(const void* data, size_t n);

struct Rd {
  const uint8_t* p = nullptr;
  size_t n = 0, off = 0;
  bool ok = true;
  Rd() {}
  Rd(const void* d, size_t len) : p((const uint8_t*)d), n(len) {}
  bool need(size_t k) { if (off + k > n) { ok = false; return false; } return true; }
  uint8_t u8() { if (!need(1)) return 0; return p[off++]; }
  uint32_t u32() { if (!need(4)) return 0; uint32_t v; memcpy(&v, p + off, 4); off += 4; return v; }
  uint64_t u64() { if (!need(8)) return 0; uint64_t v; memcpy(&v, p + off, 8); off += 8; return v; }
  int64_t i64() { return (int64_t)u64(); }
  float f32() { if (!need(4)) return 0; float v; memcpy(&v, p + off, 4); off += 4; return v; }
  std::string str() {
    uint32_t len = u32();
    if (!need(len)) return std::string();
    std::string s((const char*)p + off, len);
    off += len;
    return s;
  }
};

struct Wal {
  int fd = -1;
  std::string path;
  size_t size = 0;
  bool open(const std::string& dir, std::string& msg);
  void close();
  bool append(const void* buf, size_t n);
  bool sync();
  bool truncate_to(size_t off);
  bool load(std::string& buf, std::string& msg);
};

// 行镜像：用于回滚与 WAL 编码
struct RowImage {
  bool present = false;
  int64_t pk = 0;
  std::vector<int64_t> nums;
  std::vector<uint32_t> strs;
  std::vector<float> vecs;
};

struct WalOp {
  uint8_t type = 0;      // 1 insert / 2 update / 3 delete
  std::string tname;     // 表名（不随 tid 变化，便于恢复）
  RowImage row;
};

struct UndoEntry {
  uint8_t type = 0;      // 0=新建槽位 1=改写旧行 2=删除旧行
  uint32_t tid = 0;
  uint32_t slot = 0;
  RowImage img;
};

struct TxnState {
  bool active = false;
  std::vector<UndoEntry> undo;
  std::vector<WalOp> ops;
};

struct Engine {
  std::vector<std::unique_ptr<Table>> tables;
  std::unordered_map<std::string, uint32_t> tmap;
  uint64_t commit_seq = 0;   // 最近一次成功提交的序号
  uint64_t base_seq = 0;     // checkpoint 覆盖到的序号
  std::string data_dir;
  Wal wal;
  TxnState txn;
  bool ann_index = false;    // CREATE VECTOR INDEX 之后置位

  Table* find(const std::string& name);
  bool open(const std::string& dir, std::string& msg);
  bool load_catalog(std::string& msg);
  bool save_catalog(std::string& msg);
  bool load_base(std::string& msg);
  bool save_base(std::string& msg);
  bool replay_wal(std::string& msg);
  bool commit(DbError& err);          // 写 WAL + fsync + 清空事务
  void rollback_txn();
  bool maybe_checkpoint(std::string& msg);
};

// 提交/回滚（exec.cpp）
bool txn_commit(Engine& e, DbError& err);
void txn_rollback(Engine& e);

bool parse_vec_literal(std::string_view lit, int dim, std::vector<float>& out, DbError& err);
int utf8_codepoints(std::string_view s);

// ---------------------------------------------------------------- 执行结果
struct ResultSet {
  bool has_rows = false;
  std::vector<std::string> headers;
  std::vector<std::string> cells;      // has_rows 时按行优先展开
  size_t cols = 0;
  long long affected = 0;
};

bool exec_stmt(Engine& e, const Stmt& s, ResultSet& out, DbError& err);

// ---------------------------------------------------------------- 输出协议
struct Proto {
  enum Mode : int { MYSQL = 0, BLOCK = 1, JSONL = 2 } mode = MYSQL;
  void ready();
  void emit(const ResultSet& rs);
  void emit_error(const DbError& e);
  static const char* float_fmt();
  static std::string fmt_float(double v);
};

// 语句切分（识别字符串/注释，按 ';' 切）
struct Splitter {
  std::string cur;
  int st = 0;        // 0 普通 1 单引号 2 双引号 3 行注释 4 块注释 5 反引号
  void feed(const char* p, size_t n, std::vector<std::string>& out);
  void finish(std::vector<std::string>& out);
};

}  // namespace mv
