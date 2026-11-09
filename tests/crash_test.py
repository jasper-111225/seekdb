#!/usr/bin/env python3
"""崩溃注入：批量提交过程中 SIGKILL，重启后校验已提交批次完整、无撕裂记录。"""
import os, random, signal, subprocess, sys, time, shutil

BIN = os.path.abspath("./minivectordb")
DB = os.path.abspath("./.crashdb")
BATCH = 100

def start():
    p = subprocess.Popen([BIN, "--data-dir", DB], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)
    p.stdout.readline()          # READY
    return p

def send(p, sql):
    """发送一条语句，读到空行为止（终止符）。"""
    p.stdin.write(sql + ";\n")
    p.stdin.flush()
    lines = []
    while True:
        line = p.stdout.readline()
        if line == "":
            return lines
        if line.strip() == "":
            return lines
        lines.append(line.rstrip("\n"))

def read_ids(p):
    lines = send(p, "SELECT id FROM t ORDER BY id")
    ids = []
    for ln in lines[1:]:          # 第一条是表头
        try:
            ids.append(int(ln.strip()))
        except ValueError:
            break
    return ids

def main():
    shutil.rmtree(DB, ignore_errors=True)
    p = start()
    send(p, "CREATE TABLE t (id BIGINT PRIMARY KEY, category VARCHAR(16), content VARCHAR(64), embedding VECTOR(4))")
    committed = 0
    for b in range(60):
        rows = ",".join("(%d,'c%d','row%d','[%d,0,0,0]')" % (b*BATCH+i+1, i%5, b*BATCH+i+1, i%7)
                        for i in range(BATCH))
        send(p, "INSERT INTO t VALUES " + rows)
        committed = b + 1
        if b == 43:
            p.send_signal(signal.SIGKILL)    # 第 44 批提交后强杀
            break
    p.wait()
    p = start()
    ids = read_ids(p)
    p.stdin.close()
    p.wait()
    expect = committed * BATCH
    ok = ids == list(range(1, expect + 1))
    print("committed batches=%d expect=%d got=%d contiguous=%s" % (committed, expect, len(ids), ok))
    # 撕裂记录：往 WAL 尾部追加垃圾，重启必须仍能恢复
    with open(os.path.join(DB, "wal.log"), "ab") as f:
        f.write(b"\xde\xad\xbe\xef" * 8)
    p = start()
    ids2 = read_ids(p)
    p.stdin.close()
    p.wait()
    print("after torn-tail got=%d same=%s" % (len(ids2), ids2 == ids))
    return 0 if (ok and ids2 == ids) else 1

if __name__ == "__main__":
    sys.exit(main())
