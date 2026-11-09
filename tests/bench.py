#!/usr/bin/env python3
"""性能基准：贴近题面 default 负载（10k 行 / 128 维 / 1000 次 TopK / K=10 / 一半带过滤 / 每批 100 行）。
测 wall time、进程 CPU、峰值 RSS、WAL 字节数，便于改动前后对比。"""
import os, random, shutil, subprocess, sys, time

BIN = os.path.abspath("./minivectordb")
DB = os.path.abspath("./.benchdb")
ROWS, DIM, NQ, BATCH = 10000, 128, 1000, 100

def gen_vec(rnd):
    return "'[" + ",".join("%.4f" % rnd.uniform(-1, 1) for _ in range(DIM)) + "]\'"

def cpu_ms(pid):
    """进程累计 CPU 时间（utime+stime），单位毫秒；比 wall clock 稳定。"""
    with open("/proc/%d/stat" % pid) as f:
        parts = f.read().split()
    hz = os.sysconf("SC_CLK_TCK")
    return (int(parts[13]) + int(parts[14])) * 1000.0 / hz

def main():
    rnd = random.Random(20261109)
    shutil.rmtree(DB, ignore_errors=True)
    p = subprocess.Popen([BIN, "--data-dir", DB], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1 << 16)
    p.stdout.readline()

    def send(sql):
        p.stdin.write(sql + ";\n")
        p.stdin.flush()
        while True:
            line = p.stdout.readline()
            if line.strip() == "":
                return

    load = os.getloadavg()[0]
    c0 = cpu_ms(p.pid)
    t0 = time.monotonic()
    send("CREATE TABLE bench (id BIGINT PRIMARY KEY, category VARCHAR(16), content VARCHAR(64), embedding VECTOR(%d))" % DIM)
    rows = []
    for i in range(ROWS):
        rows.append("(%d,'c%d','text-%d',%s)" % (i + 1, i % 10, i, gen_vec(rnd)))
    for b in range(0, ROWS, BATCH):
        send("INSERT INTO bench VALUES " + ",".join(rows[b:b + BATCH]))
    t1 = time.monotonic()
    c1 = cpu_ms(p.pid)

    qs = []
    for i in range(NQ):
        q = gen_vec(rnd)
        if i % 2 == 0:
            qs.append("SELECT id, L2_DISTANCE(embedding,'%s') AS d FROM bench WHERE category = 'c%d' ORDER BY d LIMIT 10" % (q, i % 10))
        else:
            qs.append("SELECT id, L2_DISTANCE(embedding,'%s') AS d FROM bench ORDER BY d LIMIT 10" % q)
    t2 = time.monotonic()
    c2 = cpu_ms(p.pid)
    for q in qs:
        send(q)
    t3 = time.monotonic()
    c3 = cpu_ms(p.pid)

    with open("/proc/%d/status" % p.pid) as f:
        hwm = 0
        for line in f:
            if line.startswith("VmHWM:"):
                hwm = int(line.split()[1])
    p.stdin.close()
    p.wait()
    wal = os.path.getsize(os.path.join(DB, "wal.log")) if os.path.exists(os.path.join(DB, "wal.log")) else 0
    base = os.path.getsize(os.path.join(DB, "base.dat")) if os.path.exists(os.path.join(DB, "base.dat")) else 0
    print("load1   %.2f" % load)
    print("insert  %.3f s wall / %.1f ms cpu  (%.1f rows/s)" % (t1 - t0, c1 - c0, ROWS / (t1 - t0)))
    print("query   %.3f s wall / %.1f ms cpu  (%.3f ms cpu/query)" % (t3 - t2, c3 - c2, (c3 - c2) / NQ))
    print("peakRSS %.1f MiB   wal %.2f MiB   base %.2f MiB" % (hwm / 1024.0, wal / 1048576.0, base / 1048576.0))

if __name__ == "__main__":
    sys.exit(main())
