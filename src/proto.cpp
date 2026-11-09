// 输出协议：三套可切换（MYSQL / BLOCK / JSONL）+ 语句切分。
// 题包未公开前默认 MYSQL：尽量贴近 mysqltest 的文本结果。
#include "simdb.h"

namespace mv {

static bool env_flag(const char* name, bool def) {
  const char* v = getenv(name);
  if (!v || !*v) return def;
  return !(v[0] == '0' || v[0] == 'n' || v[0] == 'N' || v[0] == 'f' || v[0] == 'F');
}

const char* Proto::float_fmt() {
  const char* v = getenv("MVDB_FLOAT_FMT");
  return (v && *v) ? v : nullptr;
}

// 默认输出最短可回读的十进制表示（与 Go/Python 的 repr 一致）。
std::string Proto::fmt_float(double v) {
  if (!std::isfinite(v)) return "NULL";
  const char* f = float_fmt();
  char buf[64];
  if (f) {
    snprintf(buf, sizeof buf, f, v);
    return std::string(buf);
  }
  for (int prec = 15; prec <= 17; prec++) {
    snprintf(buf, sizeof buf, "%.*g", prec, v);
    double back = strtod(buf, nullptr);
    if (back == v) return std::string(buf);
  }
  snprintf(buf, sizeof buf, "%.17g", v);
  return std::string(buf);
}

void Proto::ready() {
  if (mode != MYSQL) return;
  if (!env_flag("MVDB_READY", true)) return;
  fputs("READY\n", stdout);
  fflush(stdout);
}

static void json_escape(std::string& out, const std::string& s) {
  out.push_back('"');
  for (unsigned char c : s) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          char b[8];
          snprintf(b, sizeof b, "\\u%04x", c);
          out += b;
        } else {
          out.push_back((char)c);
        }
    }
  }
  out.push_back('"');
}

// 每条语句的输出之后追加终止符（默认空行）。
// 驱动约定的符号未知，可用 MVDB_TERM="" 关闭，或 MVDB_TERM="OK" 之类替换。
static const char* term() {
  const char* v = getenv("MVDB_TERM");
  if (v) return v;
  return "\n";
}

void Proto::emit(const ResultSet& rs) {
  std::string o;
  if (mode == MYSQL) {
    if (rs.has_rows) {
      for (size_t i = 0; i < rs.headers.size(); i++) {
        if (i) o.push_back('\t');
        o += rs.headers[i];
      }
      o.push_back('\n');
      for (size_t r = 0; r * rs.cols < rs.cells.size() && rs.cols > 0; r++) {
        for (size_t c = 0; c < rs.cols; c++) {
          if (c) o.push_back('\t');
          o += rs.cells[r * rs.cols + c];
        }
        o.push_back('\n');
      }
    } else {
      char buf[96];
      snprintf(buf, sizeof buf, "Query OK, %lld row%s affected\n", rs.affected,
               rs.affected == 1 ? "" : "s");
      o = buf;
    }
  } else if (mode == BLOCK) {
    char buf[64];
    snprintf(buf, sizeof buf, "BEGIN %zu\n", rs.cols);
    o = buf;
    if (rs.has_rows) {
      for (size_t i = 0; i < rs.headers.size(); i++) {
        if (i) o.push_back('\t');
        o += rs.headers[i];
      }
      o.push_back('\n');
      for (size_t r = 0; r * rs.cols < rs.cells.size() && rs.cols > 0; r++) {
        for (size_t c = 0; c < rs.cols; c++) {
          if (c) o.push_back('\t');
          o += rs.cells[r * rs.cols + c];
        }
        o.push_back('\n');
      }
    }
    o += "END\n";
  } else {
    o = "{\"cols\":[";
    for (size_t i = 0; i < rs.headers.size(); i++) {
      if (i) o.push_back(',');
      json_escape(o, rs.headers[i]);
    }
    o += "],\"rows\":[";
    if (rs.has_rows && rs.cols > 0) {
      for (size_t r = 0; r * rs.cols < rs.cells.size(); r++) {
        if (r) o.push_back(',');
        o.push_back('[');
        for (size_t c = 0; c < rs.cols; c++) {
          if (c) o.push_back(',');
          json_escape(o, rs.cells[r * rs.cols + c]);
        }
        o.push_back(']');
      }
    }
    o += "],\"affected\":";
    char buf[32];
    snprintf(buf, sizeof buf, "%lld", rs.affected);
    o += buf;
    o += "}\n";
  }
  o += term();
  fwrite(o.data(), 1, o.size(), stdout);
  fflush(stdout);
}

const char* semantic_of(int code, const std::string& msg) {
  if (msg.find("dimension mismatch") != std::string::npos) return "dimension_mismatch";
  if (msg.find("vector") != std::string::npos) return "invalid_vector";
  switch (code) {
    case E_DUP_KEY: return "duplicate_key";
    case E_NO_TABLE: return "unknown_table";
    case E_TABLE_EXISTS: return "table_exists";
    case E_BAD_COL: return "unknown_column";
    case E_DUP_COL: return "duplicate_column";
    case E_BAD_COUNT: return "wrong_value_count";
    case E_OUT_OF_RANGE: return "value_out_of_range";
    case E_TOO_LONG: return "value_too_long";
    case E_UNSUPPORTED: return "unsupported_sql";
    case E_SYNTAX: return "syntax_error";
    default: break;
  }
  if (msg.find("not supported") != std::string::npos ||
      msg.find("unsupported") != std::string::npos) return "unsupported_sql";
  if (msg.find("Duplicate entry") != std::string::npos) return "duplicate_key";
  return "error";
}

void Proto::emit_error(const DbError& e) {
  std::string o;
  char buf[64];
  if (mode == JSONL) {
    snprintf(buf, sizeof buf, "{\"error\":{\"code\":%d,", e.code);
    o = buf;
    o += "\"sqlstate\":";
    json_escape(o, sqlstate_of(e.code));
    o += ",\"message\":";
    json_escape(o, e.msg);
    o += "}}\n";
  } else {
    snprintf(buf, sizeof buf, "ERROR %d (%s) [%s]: ", e.code, sqlstate_of(e.code),
             semantic_of(e.code, e.msg));
    o = buf;
    o += e.msg;
    o.push_back('\n');
  }
  o += term();
  fwrite(o.data(), 1, o.size(), stdout);
  fflush(stdout);
}

// ---------------------------------------------------------------- 切分
void Splitter::feed(const char* p, size_t n, std::vector<std::string>& out) {
  for (size_t i = 0; i < n; i++) {
    char c = p[i];
    switch (st) {
      case 0:
        if (c == '\'') { st = 1; cur.push_back(c); }
        else if (c == '"') { st = 2; cur.push_back(c); }
        else if (c == '`') { st = 5; cur.push_back(c); }
        else if (c == '-' && i + 1 < n && p[i + 1] == '-') { st = 3; cur.push_back(c); }
        else if (c == '#') { st = 3; cur.push_back(c); }
        else if (c == '/' && i + 1 < n && p[i + 1] == '*') { st = 4; cur.push_back(c); }
        else if (c == ';') { out.push_back(cur); cur.clear(); }
        else if (c == '\\' && i + 1 < n && p[i + 1] == 'g') {
          // mysql 客户端风格的 \g 结束符
          i++;
          out.push_back(cur);
          cur.clear();
        } else cur.push_back(c);
        break;
      case 1:
        cur.push_back(c);
        if (c == '\\' && i + 1 < n) { cur.push_back(p[++i]); }
        else if (c == '\'') st = 0;
        break;
      case 2:
        cur.push_back(c);
        if (c == '\\' && i + 1 < n) { cur.push_back(p[++i]); }
        else if (c == '"') st = 0;
        break;
      case 5:
        cur.push_back(c);
        if (c == '`') st = 0;
        break;
      case 3:
        cur.push_back(c);
        if (c == '\n') st = 0;
        break;
      case 4:
        cur.push_back(c);
        if (c == '*' && i + 1 < n && p[i + 1] == '/') { cur.push_back(p[++i]); st = 0; }
        break;
    }
  }
}

void Splitter::finish(std::vector<std::string>& out) {
  out.push_back(cur);
  cur.clear();
}

}  // namespace mv
