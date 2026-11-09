#!/usr/bin/env python3
"""向量 TopK 差分校验：与 Python 暴力解（按 float32 量化后）逐项比对，含并列主键序。"""
import math, os, random, shutil, struct, subprocess, sys

BIN = os.path.abspath("./minivectordb")
DB = os.path.abspath("./.topkdb")
DIM = 8
N = 300

def f32(x):
    return struct.unpack("f", struct.pack("f", x))[0]

def run(sqls):
    p = subprocess.Popen([BIN, "--data-dir", DB], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL, text=True, bufsize=1 << 16)
    p.stdout.readline()
    res = []
    for sql in sqls:
        p.stdin.write(sql + ";\n")
        p.stdin.flush()
        lines = []
        while True:
            ln = p.stdout.readline()
            if ln.strip() == "":
                break
            lines.append(ln.rstrip("\n"))
        res.append(lines)
    p.stdin.close()
    p.wait()
    return res

def main():
    rnd = random.Random(7)
    shutil.rmtree(DB, ignore_errors=True)
    rows = []
    for i in range(1, N + 1):
        v = [f32(rnd.choice([-1.0, 0.0, 0.5, 0.25]) + f32(rnd.uniform(-0.01, 0.01))) for _ in range(DIM)]
        rows.append((i, "c%d" % (i % 4), v))
    sql = ["CREATE TABLE t (id BIGINT PRIMARY KEY, category VARCHAR(8), embedding VECTOR(%d))" % DIM]
    for b in range(0, N, 50):
        vals = ",".join("(%d,'%s','[%s]')" % (i, c, ",".join("%.9g" % x for x in v)) for i, c, v in rows[b:b + 50])
        sql.append("INSERT INTO t VALUES " + vals)
    queries = []
    for k in range(12):
        q = [f32(rnd.uniform(-1, 1)) for _ in range(DIM)]
        cat = "c%d" % (k % 4) if k % 2 == 0 else None
        lit = "'[%s]'" % ",".join("%.9g" % x for x in q)
        where = " WHERE category = '%s'" % cat if cat else ""
        sql.append("SELECT id, L2_DISTANCE(embedding, %s) AS d FROM t%s ORDER BY d LIMIT 10" % (lit, where))
        queries.append((q, cat))
    out = run(sql)
    bad = 0
    for (q, cat), lines in zip(queries, out[len(sql) - len(queries):]):
        got = [int(l.split("\t")[0]) for l in lines[1:]]
        cand = [(i, v) for i, c, v in rows if cat is None or c == cat]
        def dist(v):
            return math.sqrt(sum((float(a) - float(b)) ** 2 for a, b in zip(q, v)))
        want = [i for i, v in sorted(cand, key=lambda t: (dist(t[1]), t[0]))][:10]
        ok = got == want
        if not ok:
            bad += 1
            print("MISMATCH q=%s cat=%s\n got=%s\nwant=%s" % (q, cat, got, want))
    print("checked %d queries, mismatch=%d" % (len(queries), bad))
    return 0 if bad == 0 else 1

if __name__ == "__main__":
    sys.exit(main())
