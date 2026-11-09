// 进程入口：前台运行，stdin 收 SQL，stdout 出结果，stderr 出日志。
#include "simdb.h"

#include <csignal>
#include <cstdlib>
#include <unistd.h>

namespace {

mv::Engine g_engine;
volatile sig_atomic_t g_stop = 0;

void on_signal(int) { g_stop = 1; }

bool parse_arg(int argc, char** argv, const char* name, std::string& out) {
  std::string key = std::string("--") + name;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == key && i + 1 < argc) { out = argv[i + 1]; return true; }
    if (a.compare(0, key.size() + 1, key + "=") == 0) { out = a.substr(key.size() + 1); return true; }
  }
  return false;
}


// --selftest：不依赖标准输入，跑一遍最小闭环（建表/写入/向量 TopK/事务回滚/重启恢复），
// 供镜像构建阶段做冒烟（构建期就能发现"二进制跑不起来"这类问题）。
int selftest() {
  char tmpl[] = "/tmp/mvdbselfXXXXXX";
  char* dir = mkdtemp(tmpl);
  if (!dir) { fprintf(stderr, "selftest: mkdtemp failed\n"); return 1; }
  int failures = 0;
  auto run = [&](mv::Engine& e, const char* sql, bool want_ok) {
    mv::Parser p; mv::Stmt st; std::string err;
    std::string q(sql);
    while (!q.empty() && (q.back() == ';' || q.back() == ' ' || q.back() == '\n')) q.pop_back();
    if (!p.parse(q, st, err)) {
      fprintf(stderr, "selftest: parse failed: %s\n", sql); failures++; return mv::ResultSet{};
    }
    mv::ResultSet rs; mv::DbError e2;
    bool ok = mv::exec_stmt(e, st, rs, e2);
    if (ok != want_ok) { fprintf(stderr, "selftest: unexpected %s for: %s\n", ok ? "ok" : "error", sql); failures++; }
    return rs;
  };
  {
    mv::Engine e; std::string msg;
    if (!e.open(dir, msg)) { fprintf(stderr, "selftest: open failed: %s\n", msg.c_str()); return 1; }
    run(e, "CREATE TABLE t (id BIGINT PRIMARY KEY, c VARCHAR(16), v VECTOR(2));", true);
    run(e, "INSERT INTO t VALUES (1,'a','[0,0]'),(2,'b','[3,4]');", true);
    mv::ResultSet rs = run(e, "SELECT L2_DISTANCE(v,'[0,0]') AS d FROM t WHERE id = 2;", true);
    if (rs.cells.empty()) { fprintf(stderr, "selftest: no distance row\n"); failures++; }
    else {
      double d = std::strtod(rs.cells[0].c_str(), nullptr);
      if (d < 4.9999 || d > 5.0001) { fprintf(stderr, "selftest: distance %s != 5\n", rs.cells[0].c_str()); failures++; }
    }
    run(e, "BEGIN;", true);
    run(e, "DELETE FROM t;", true);
    run(e, "ROLLBACK;", true);
    rs = run(e, "SELECT COUNT(*) FROM t;", true);
    if (rs.cells.empty() || rs.cells[0] != "2") { fprintf(stderr, "selftest: rollback mismatch\n"); failures++; }
    run(e, "INSERT INTO t VALUES (2,'b','[3,4]');", false);  // 重复主键必须失败
  }
  {  // 重开验证持久化
    mv::Engine e; std::string msg;
    if (!e.open(dir, msg)) { fprintf(stderr, "selftest: reopen failed: %s\n", msg.c_str()); return 1; }
    mv::ResultSet rs = run(e, "SELECT COUNT(*) FROM t;", true);
    if (rs.cells.empty() || rs.cells[0] != "2") { fprintf(stderr, "selftest: recover mismatch\n"); failures++; }
  }
  std::string cleanup = std::string("rm -rf '") + dir + "'";
  if (system(cleanup.c_str()) != 0) { /* 清理失败不影响结论 */ }
  fprintf(stderr, "selftest: %d failure(s)\n", failures);
  return failures == 0 ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) {
  std::string data_dir = "/data";
  std::string proto = "mysql";
  std::string v;
  for (int i = 1; i < argc; i++) {
    if (std::string(argv[i]) == "--selftest") return selftest();
  }
  if (parse_arg(argc, argv, "data-dir", v)) data_dir = v;
  if (parse_arg(argc, argv, "proto", v)) proto = v;
  if (parse_arg(argc, argv, "mode", v)) {
    // exact / ann：当前实现恒为精确检索，ann 仅作为语义开关接受
    if (v != "exact" && v != "ann") {
      fprintf(stderr, "unknown mode: %s\n", v.c_str());
      return 2;
    }
  }
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--help" || a == "-h") {
      fprintf(stderr,
              "usage: minivectordb [--data-dir DIR] [--proto mysql|block|jsonl] "
              "[--mode exact|ann]\n");
      return 0;
    }
  }
  mv::Proto out;
  if (proto == "block") out.mode = mv::Proto::BLOCK;
  else if (proto == "jsonl") out.mode = mv::Proto::JSONL;
  else if (proto == "mysql") out.mode = mv::Proto::MYSQL;
  else {
    fprintf(stderr, "unknown proto: %s\n", proto.c_str());
    return 2;
  }

  std::string msg;
  if (!g_engine.open(data_dir, msg)) {
    fprintf(stderr, "open %s failed: %s\n", data_dir.c_str(), msg.c_str());
    return 1;
  }
  signal(SIGINT, on_signal);
  signal(SIGTERM, on_signal);
  signal(SIGPIPE, SIG_IGN);

  out.ready();
  fprintf(stderr, "minivectordb: data-dir=%s proto=%s simd=%d\n", data_dir.c_str(), proto.c_str(),
          mv::simd_level());

  mv::Parser parser;
  mv::Splitter splitter;
  std::vector<std::string> stmts;
  std::vector<char> buf(1 << 16);
  bool running = true;
  while (running && !g_stop) {
    ssize_t n = ::read(0, buf.data(), buf.size());
    if (n < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (n == 0) {
      splitter.finish(stmts);
    } else {
      splitter.feed(buf.data(), (size_t)n, stmts);
    }
    bool eof = (n == 0);
    for (auto& sql : stmts) {
      bool only_space = true;
      for (char c : sql) {
        if (!(c == ' ' || c == '\t' || c == '\r' || c == '\n')) { only_space = false; break; }
      }
      if (only_space) continue;
      mv::Stmt st;
      std::string perr;
      if (!parser.parse(sql, st, perr)) {
        out.emit_error(mv::AE(mv::E_SYNTAX, perr));
        continue;
      }
      if (st.k == mv::Stmt::EXIT) { running = false; break; }
      mv::ResultSet rs;
      mv::DbError err;
      if (!mv::exec_stmt(g_engine, st, rs, err)) out.emit_error(err);
      else out.emit(rs);
    }
    stmts.clear();
    if (eof) break;
  }

  // 正常关闭：回滚未提交事务；已提交数据在 commit 时已 fsync
  if (g_engine.txn.active || !g_engine.txn.ops.empty()) {
    g_engine.rollback_txn();
  }
  g_engine.wal.close();
  fflush(stdout);
  return 0;
}
