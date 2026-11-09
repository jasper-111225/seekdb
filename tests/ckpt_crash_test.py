#!/usr/bin/env python3
"""小阈值 checkpoint 下的崩溃恢复：每批 100 行提交，随机时点 SIGKILL，重启后必须是 100 的整数倍且连续。

checkpoint 落在提交路径上（base.dat 写 tmp -> fsync -> rename -> 再 truncate WAL），
所以杀在任意时刻都要么看到旧 base + 完整 WAL，要么看到新 base + 已截断 WAL，不能出现半截。
"""
import os, random, shutil, signal, subprocess, sys, time

BIN = "./minivectordb"
DIR = ".ckptdb"
BATCH, NB = 100, 40
random.seed(7)


def start():
    env = dict(os.environ, MVDB_CKPT_BYTES="65536", MVDB_READY="0")
    return subprocess.Popen([BIN, "--data-dir", DIR], stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, env=env, text=True)


def send(p, sql, want_rows=False):
    p.stdin.write(sql + "\n")
    p.stdin.flush()
    out = []
    while True:
        line = p.stdout.readline()
        if line == "":
            raise RuntimeError("process died")
        line = line.rstrip("\n")
        if line == "":
            return out
        out.append(line)


def main():
    shutil.rmtree(DIR, ignore_errors=True)
    p = start()
    send(p, "CREATE TABLE t (id BIGINT PRIMARY KEY, c VARCHAR(8), v VECTOR(4));")
    committed = 0
    kill_at = random.randint(5, NB)
    for b in range(NB):
        rows = ",".join("(%d,'r%d','[%d.5,0,0,0]')" % (b * BATCH + i, i, i) for i in range(BATCH))
        send(p, "INSERT INTO t VALUES %s;" % rows)
        committed += BATCH
        if b == kill_at:
            p.send_signal(signal.SIGKILL)
            p.wait()
            break
    # 重启：行数必须是 100 的整数倍，且 id 连续（无洞）
    p = start()
    out = send(p, "SELECT COUNT(*) FROM t;")
    n = int(out[1])
    if n % BATCH != 0:
        print("FAIL count=%d not a multiple of %d" % (n, BATCH)); return 1
    out = send(p, "SELECT id FROM t ORDER BY id;")
    ids = [int(x) for x in out[1:]]
    if ids != list(range(0, n)):
        print("FAIL ids not contiguous: %d..%d n=%d" % (ids[0], ids[-1], n)); return 1
    # 再写一批并正常关闭，确认 checkpoint 之后继续工作
    send(p, "INSERT INTO t VALUES (%d,'tail','[0,0,0,0]');" % n)
    p.stdin.close(); p.wait(timeout=10)
    p = start()
    out = send(p, "SELECT COUNT(*) FROM t;")
    if int(out[1]) != n + 1:
        print("FAIL after graceful exit count=%s expect %d" % (out[1], n + 1)); return 1
    p.stdin.close(); p.wait(timeout=10)
    print("OK count=%d (multiple of %d, contiguous), survived kill at batch %d, checkpoint=64KB"
          % (n, BATCH, kill_at))
    return 0


if __name__ == "__main__":
    sys.exit(main())
