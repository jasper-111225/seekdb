// 持久化：WAL（追加 + crc + fsync）与全量 checkpoint（base.dat），
// 以及恢复流程与提交/回滚的落盘部分。
#include "simdb.h"

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

namespace mv {

// ---------------------------------------------------------------- 常量
static const uint32_t WAL_MAGIC = 0x5732564Du;   // "MV2W"
static const uint32_t BASE_MAGIC = 0x4232564Du;  // "MV2B"
// WAL 阈值：默认 32MB，可用 MVDB_CKPT_BYTES 覆盖（测试小阈值下的 checkpoint 路径）
static size_t ckpt_bytes() {
  static size_t v = 0;
  if (!v) {
    const char* s = getenv("MVDB_CKPT_BYTES");
    v = s ? (size_t)strtoull(s, nullptr, 10) : (32u << 20);
    if (v < 4096) v = 4096;
  }
  return v;
}
static const uint32_t MAX_WAL_PAYLOAD = 64u << 20;

// ---------------------------------------------------------------- 错误码
const char* sqlstate_of(int code) {
  switch (code) {
    case E_NO_TABLE: return "42S02";
    case E_TABLE_EXISTS: return "42S01";
    case E_BAD_COL: return "42S22";
    case E_DUP_COL: return "42S21";
    case E_DUP_KEY: return "23000";
    case E_SYNTAX: return "42000";
    case E_BAD_COUNT: return "21S01";
    case E_BAD_VALUE: return "HY000";
    case E_OUT_OF_RANGE: return "22003";
    case E_TOO_LONG: return "22001";
    case E_UNSUPPORTED: return "42000";
    default: return "HY000";
  }
}

const char* ty_name(Ty t) {
  switch (t) {
    case Ty::BIGINT: return "BIGINT";
    case Ty::VARCHAR: return "VARCHAR";
    case Ty::VECTOR: return "VECTOR";
  }
  return "?";
}

// ---------------------------------------------------------------- 编码
void put_u8(std::string& b, uint8_t v) { b.push_back((char)v); }
void put_u32(std::string& b, uint32_t v) { b.append((const char*)&v, 4); }
void put_u64(std::string& b, uint64_t v) { b.append((const char*)&v, 8); }
void put_i64(std::string& b, int64_t v) { b.append((const char*)&v, 8); }
void put_f32(std::string& b, float v) { b.append((const char*)&v, 4); }
void put_str(std::string& b, std::string_view s) {
  put_u32(b, (uint32_t)s.size());
  b.append(s.data(), s.size());
}

uint32_t crc32c(const void* data, size_t n) {
  static uint32_t tab[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) c = (c & 1) ? (0x82F63B78u ^ (c >> 1)) : (c >> 1);
      tab[i] = c;
    }
    init = true;
  }
  const uint8_t* p = (const uint8_t*)data;
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) c = tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

// ---------------------------------------------------------------- 目录工具
static bool mkdirs(const std::string& path) {
  std::string cur;
  for (size_t i = 0; i < path.size(); i++) {
    cur.push_back(path[i]);
    if (path[i] == '/' || i + 1 == path.size()) {
      if (cur == "/" || cur.empty()) continue;
      std::string p = cur;
      while (!p.empty() && p.back() == '/') p.pop_back();
      if (p.empty()) continue;
      if (::mkdir(p.c_str(), 0755) != 0 && errno != EEXIST) return false;
    }
  }
  return true;
}

static bool write_all(int fd, const void* buf, size_t n) {
  const char* p = (const char*)buf;
  while (n > 0) {
    ssize_t w = ::write(fd, p, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    p += w;
    n -= (size_t)w;
  }
  return true;
}

static bool read_file(const std::string& path, std::string& out) {
  int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  out.clear();
  char buf[1 << 16];
  while (true) {
    ssize_t r = ::read(fd, buf, sizeof(buf));
    if (r < 0) {
      if (errno == EINTR) continue;
      ::close(fd);
      return false;
    }
    if (r == 0) break;
    out.append(buf, (size_t)r);
  }
  ::close(fd);
  return true;
}

static bool fsync_dir(const std::string& dir) {
  int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return false;
  bool ok = ::fsync(fd) == 0;
  ::close(fd);
  return ok;
}

// ---------------------------------------------------------------- WAL
bool Wal::open(const std::string& dir, std::string& msg) {
  if (!mkdirs(dir)) { msg = "cannot create data dir " + dir; return false; }
  path = dir + "/wal.log";
  fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
  if (fd < 0) { msg = "cannot open wal: " + std::string(strerror(errno)); return false; }
  size = 0;
  struct stat st;
  if (::fstat(fd, &st) == 0) size = (size_t)st.st_size;
  return true;
}

void Wal::close() {
  if (fd >= 0) { ::close(fd); fd = -1; }
}

bool Wal::append(const void* buf, size_t n) {
  if (fd < 0) return false;
  if (!write_all(fd, buf, n)) return false;
  size += n;
  return true;
}

bool Wal::sync() {
  if (fd < 0) return false;
  return ::fdatasync(fd) == 0;
}

bool Wal::truncate_to(size_t off) {
  if (fd < 0) return false;
  if (::ftruncate(fd, (off_t)off) != 0) return false;
  if (::lseek(fd, (off_t)off, SEEK_SET) < 0) return false;
  size = off;
  return true;
}

bool Wal::load(std::string& buf, std::string& msg) {
  buf.clear();
  if (!read_file(path, buf)) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return true;   // 文件还不存在 = 空 WAL
    msg = "cannot read wal";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------- 行镜像
static void row_to_slot(Table& t, uint32_t slot, const RowImage& img) {
  const Schema& s = t.sch;
  t.pk[slot] = img.pk;
  if (s.nnum) memcpy(&t.nums[(size_t)slot * s.nnum], img.nums.data(), sizeof(int64_t) * s.nnum);
  if (s.nstr) memcpy(&t.strs[(size_t)slot * s.nstr], img.strs.data(), sizeof(uint32_t) * s.nstr);
  if (s.dim) memcpy(&t.vecs[(size_t)slot * s.dim], img.vecs.data(), sizeof(float) * s.dim);
}

// ---------------------------------------------------------------- 表/恢复辅助
Table* Engine::find(const std::string& name) {
  auto it = tmap.find(name);
  if (it == tmap.end()) return nullptr;
  Table* t = tables[it->second].get();
  if (!t || t->dropped) return nullptr;
  return t;
}

// ---------------------------------------------------------------- base.dat
bool Engine::save_base(std::string& msg) {
  std::string b;
  put_u32(b, BASE_MAGIC);
  put_u32(b, 1);
  put_u64(b, commit_seq);
  uint32_t nt = 0;
  for (auto& tp : tables) if (tp && !tp->dropped) nt++;
  put_u32(b, nt);
  for (auto& tp : tables) {
    if (!tp || tp->dropped) continue;
    Table& t = *tp;
    const Schema& s = t.sch;
    put_str(b, s.name);
    put_u8(b, (uint8_t)(s.pk < 0 ? 255 : s.pk));
    put_u8(b, (uint8_t)(s.vec < 0 ? 255 : s.vec));
    put_u32(b, (uint32_t)s.cols.size());
    for (auto& c : s.cols) {
      put_str(b, c.name);
      put_u8(b, (uint8_t)c.ty);
      put_u32(b, (uint32_t)c.n);
    }
    put_u32(b, t.nslots);
    put_u32(b, (uint32_t)t.dict.size());
    for (auto& d : t.dict) put_str(b, d);
    uint32_t live = 0;
    for (uint32_t i = 0; i < t.nslots; i++) if (t.used[i]) live++;
    put_u32(b, live);
    std::string rowbuf;
    for (uint32_t i = 0; i < t.nslots; i++) {
      if (!t.used[i]) continue;
      rowbuf.clear();
      put_i64(rowbuf, t.pk[i]);
      for (int k = 0; k < s.nnum; k++) put_i64(rowbuf, t.nums[(size_t)i * s.nnum + k]);
      for (int k = 0; k < s.nstr; k++) put_u32(rowbuf, t.strs[(size_t)i * s.nstr + k]);
      for (int k = 0; k < s.dim; k++) put_f32(rowbuf, t.vecs[(size_t)i * s.dim + k]);
      put_u32(b, (uint32_t)rowbuf.size());
      b.append(rowbuf);
    }
  }
  std::string tmp = data_dir + "/base.dat.tmp";
  int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) { msg = "cannot write base.dat"; return false; }
  bool ok = write_all(fd, b.data(), b.size()) && ::fsync(fd) == 0;
  ::close(fd);
  if (!ok) { ::unlink(tmp.c_str()); msg = "base.dat write failed"; return false; }
  if (::rename(tmp.c_str(), (data_dir + "/base.dat").c_str()) != 0) {
    msg = "base.dat rename failed";
    return false;
  }
  fsync_dir(data_dir);
  base_seq = commit_seq;
  // base.dat 已覆盖此前所有提交，WAL 可以从头开始
  if (wal.fd >= 0) {
    wal.truncate_to(0);
    wal.sync();
  }
  return true;
}

bool Engine::load_base(std::string& msg) {
  std::string b;
  if (!read_file(data_dir + "/base.dat", b)) return true;   // 无 checkpoint
  Rd r(b.data(), b.size());
  if (r.u32() != BASE_MAGIC) { msg = "bad base.dat magic"; return false; }
  if (r.u32() != 1) { msg = "unsupported base.dat version"; return false; }
  base_seq = r.u64();
  uint32_t nt = r.u32();
  if (!r.ok) { msg = "truncated base.dat"; return false; }
  for (uint32_t ti = 0; ti < nt; ti++) {
    std::string name = r.str();
    int pk = (int)r.u8();
    int vec = (int)r.u8();
    if (pk == 255) pk = -1;
    if (vec == 255) vec = -1;
    uint32_t ncols = r.u32();
    Schema s;
    s.name = name;
    for (uint32_t ci = 0; ci < ncols; ci++) {
      ColDef c;
      c.name = r.str();
      c.ty = (Ty)r.u8();
      c.n = (int)r.u32();
      s.cols.push_back(std::move(c));
    }
    s.pk = pk;
    s.vec = vec;
    s.finalize();
    if (!r.ok) { msg = "truncated base.dat schema"; return false; }
    std::unique_ptr<Table> t(new Table());
    t->sch = std::move(s);
    uint32_t nslots = r.u32();
    uint32_t ndict = r.u32();
    for (uint32_t di = 0; di < ndict; di++) {
      std::string d = r.str();
      t->dictmap.emplace(d, (uint32_t)t->dict.size());
      t->dict.push_back(std::move(d));
    }
    uint32_t live = r.u32();
    if (!r.ok) { msg = "truncated base.dat table"; return false; }
    t->reserve_slots(nslots);
    for (uint32_t i = 0; i < live; i++) {
      uint32_t rl = r.u32();
      if (!r.need(rl)) { msg = "truncated base.dat row"; return false; }
      size_t start = r.off;
      uint32_t slot = t->nslots;
      t->reserve_slots(slot + 1);
      t->nslots = slot + 1;
      t->used[slot] = 1;
      t->pk[slot] = r.i64();
      const Schema& sc = t->sch;
      for (int k = 0; k < sc.nnum; k++) t->nums[(size_t)slot * sc.nnum + k] = r.i64();
      for (int k = 0; k < sc.nstr; k++) t->strs[(size_t)slot * sc.nstr + k] = r.u32();
      for (int k = 0; k < sc.dim; k++) t->vecs[(size_t)slot * sc.dim + k] = r.f32();
      t->pkmap[t->pk[slot]] = slot;
      r.off = start + rl;
    }
    t->live = live;
    uint32_t id = (uint32_t)tables.size();
    tmap[name] = id;
    tables.push_back(std::move(t));
  }
  commit_seq = base_seq;
  return true;
}

// ---------------------------------------------------------------- WAL 重放
bool Engine::replay_wal(std::string& msg) {
  std::string buf;
  if (!wal.load(buf, msg)) return false;
  size_t off = 0, good = 0;
  uint64_t maxseq = 0;
  while (off + 12 <= buf.size()) {
    Rd hdr(buf.data() + off, buf.size() - off);
    uint32_t magic = hdr.u32();
    uint32_t crc = hdr.u32();
    uint32_t len = hdr.u32();
    if (magic != WAL_MAGIC || len > MAX_WAL_PAYLOAD || off + 12 + len > buf.size()) break;
    const char* payload = buf.data() + off + 12;
    if (crc32c(payload, len) != crc) break;
    Rd r(payload, len);
    uint64_t seq = r.u64();
    uint32_t nops = r.u32();
    for (uint32_t i = 0; i < nops && r.ok; i++) {
      uint8_t type = r.u8();
      std::string tname = r.str();
      int64_t pk = r.i64();
      if (!r.ok) break;
      Table* t = find(tname);
      if (!t) continue;
      const Schema& sc = t->sch;
      if (type == 3) {
        auto it = t->pkmap.find(pk);
        if (it != t->pkmap.end()) {
          uint32_t slot = it->second;
          t->used[slot] = 0;
          t->live--;
          t->pkmap.erase(it);
          t->freelist.push_back(slot);
        }
        continue;
      }
      RowImage img;
      img.present = true;
      img.pk = pk;
      img.nums.resize(sc.nnum);
      img.strs.resize(sc.nstr);
      img.vecs.resize(sc.dim);
      for (int k = 0; k < sc.nnum; k++) img.nums[k] = r.i64();
      for (int k = 0; k < sc.nstr; k++) {
        std::string sv = r.str();
        img.strs[k] = t->dict_intern(sv);
      }
      for (int k = 0; k < sc.dim; k++) img.vecs[k] = r.f32();
      if (!r.ok) break;
      auto it = t->pkmap.find(pk);
      if (it != t->pkmap.end()) {
        row_to_slot(*t, it->second, img);
      } else {
        uint32_t slot = t->alloc_slot();
        row_to_slot(*t, slot, img);
        t->live++;
        t->pkmap[pk] = slot;
      }
    }
    if (!r.ok) break;
    if (seq > maxseq) maxseq = seq;
    off += 12 + len;
    good = off;
  }
  // 截断撕裂/损坏的尾部记录，保证下次恢复得到同样结果
  if (good < buf.size()) {
    if (!wal.truncate_to(good)) { msg = "cannot truncate torn wal"; return false; }
    if (!wal.sync()) { msg = "cannot sync wal after truncate"; return false; }
  }
  if (maxseq > commit_seq) commit_seq = maxseq;
  return true;
}

// ---------------------------------------------------------------- 打开
bool Engine::open(const std::string& dir, std::string& msg) {
  data_dir = dir;
  if (!mkdirs(dir)) { msg = "cannot create data dir: " + dir; return false; }
  if (!wal.open(dir, msg)) return false;
  if (!load_base(msg)) return false;
  if (!replay_wal(msg)) return false;
  return true;
}

// ---------------------------------------------------------------- 提交
bool Engine::commit(DbError& err) {
  if (txn.ops.empty()) {
    txn.active = false;
    txn.undo.clear();
    return true;
  }
  std::string payload;
  put_u64(payload, commit_seq + 1);
  put_u32(payload, (uint32_t)txn.ops.size());
  for (auto& op : txn.ops) {
    put_u8(payload, op.type);
    put_str(payload, op.tname);
    put_i64(payload, op.row.pk);
    if (op.type == 3) continue;
    for (int64_t v : op.row.nums) put_i64(payload, v);
    // 字符串按内容落盘：字典码只在当前进程内有效
    Table* ot = find(op.tname);
    for (uint32_t code : op.row.strs) {
      if (ot && code < ot->dict.size()) put_str(payload, ot->dict_get(code));
      else put_str(payload, std::string_view());
    }
    for (float v : op.row.vecs) put_f32(payload, v);
  }
  std::string rec;
  put_u32(rec, WAL_MAGIC);
  put_u32(rec, crc32c(payload.data(), payload.size()));
  put_u32(rec, (uint32_t)payload.size());
  rec.append(payload);
  if (!wal.append(rec.data(), rec.size()) || !wal.sync()) {
    err = AE(E_GENERIC, "write-ahead log append failed");
    rollback_txn();
    return false;
  }
  commit_seq++;
  txn.ops.clear();
  txn.undo.clear();
  txn.active = false;
  std::string m;
  if (!maybe_checkpoint(m)) {
    err = AE(E_GENERIC, "checkpoint failed: " + m);
    return false;
  }
  return true;
}

bool Engine::maybe_checkpoint(std::string& msg) {
  if (wal.size < ckpt_bytes()) return true;
  if (!save_base(msg)) return false;
  if (!wal.truncate_to(0) || !wal.sync()) { msg = "truncate wal failed"; return false; }
  return true;
}

}  // namespace mv
