// 进程入口：前台运行，stdin 收 SQL，stdout 出结果，stderr 出日志。
#include "simdb.h"

#include <csignal>
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

}  // namespace

int main(int argc, char** argv) {
  std::string data_dir = "/data";
  std::string proto = "mysql";
  std::string v;
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
