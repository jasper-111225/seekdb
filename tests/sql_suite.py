#!/usr/bin/env python3
"""SQL 语义回归：DDL/DML/事务/重启恢复/边界值/错误码。逐条断言，失败即打印上下文。"""
import os, re, shutil, subprocess, sys

BIN = os.path.abspath("./minivectordb")
DB = os.path.abspath("./.suitdb")
fails = []

class Sess:
    def __init__(self):
        self.p = subprocess.Popen([BIN, "--data-dir", DB], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.DEVNULL, text=True, bufsize=1 << 16)
        self.p.stdout.readline()          # READY

    def send(self, sql):
        self.p.stdin.write(sql + ";\n")
        self.p.stdin.flush()
        lines = []
        while True:
            ln = self.p.stdout.readline()
            if ln.strip() == "":
                break
            lines.append(ln.rstrip("\n"))
        if lines and lines[0].startswith("ERROR "):
            m = re.match(r"ERROR (\d+)", lines[0])
            return ("err", int(m.group(1)), lines[0])
        if lines and lines[0].startswith("Query OK"):
            m = re.search(r"Query OK, (\d+) row", lines[0])
            return ("ok", int(m.group(1)) if m else 0)
        return ("rows", [l.split("\t") for l in lines])

    def rows(self, sql):
        r = self.send(sql)
        assert r[0] == "rows", (sql, r)
        return r[1][1:]

    def close(self):
        self.p.stdin.close()
        self.p.wait()

def check(name, cond, detail=""):
    if not cond:
        fails.append("%s  %s" % (name, detail))
        print("FAIL", name, detail)

def main():
    shutil.rmtree(DB, ignore_errors=True)
    s = Sess()
    s.send("DROP TABLE IF EXISTS t")           # 不支持 IF EXISTS 时也接受错误
    check("create", s.send("CREATE TABLE t (id BIGINT PRIMARY KEY, name VARCHAR(8), v VECTOR(3))")[0] == "ok")
    check("create dup", s.send("CREATE TABLE t (id BIGINT PRIMARY KEY)")[1] == 1050)
    check("desc", [r[0] for r in s.rows("DESC t")] == ["id", "name", "v"])
    check("show tables", len(s.rows("SHOW TABLES")) == 1)

    s.send("INSERT INTO t VALUES (1,'ab','[1,0,0]'),(2,'cd','[0,1,0]')")
    check("dup pk", s.send("INSERT INTO t VALUES (1,'x','[0,0,1]')")[1] == 1062)
    check("count mismatch", s.send("INSERT INTO t VALUES (3,'x')")[1] == 1136)
    check("too long", s.send("INSERT INTO t VALUES (3,'abcdefghi','[0,0,1]')")[1] == 1406)
    check("bad vector dim", s.send("INSERT INTO t VALUES (3,'x','[0,1]')")[1] == 1366)
    check("bad vector nan", s.send("INSERT INTO t VALUES (3,'x','[nan,0,0]')")[1] == 1366)
    check("bad vector inf", s.send("INSERT INTO t VALUES (3,'x','[inf,0,0]')")[1] == 1366)
    check("bad type", s.send("INSERT INTO t VALUES (3,42,'[0,0,1]')")[1] == 1366)
    check("unknown col", s.send("SELECT nope FROM t")[1] == 1054)
    check("unknown table", s.send("SELECT * FROM nope")[1] == 1146)
    check("syntax", s.send("SELEC 1")[1] == 1064)

    check("select all", len(s.rows("SELECT id FROM t")) == 2)
    check("where eq", s.rows("SELECT id FROM t WHERE name = 'cd'") == [["2"]])
    check("where ne", len(s.rows("SELECT id FROM t WHERE name <> 'cd'")) == 1)
    check("where and", s.rows("SELECT id FROM t WHERE id >= 2 AND name = 'cd'") == [["2"]])
    check("order by desc", [r[0] for r in s.rows("SELECT id FROM t ORDER BY id DESC")] == ["2", "1"])

    # 事务：回滚
    s.send("BEGIN")
    s.send("UPDATE t SET name = 'zz' WHERE id = 1")
    s.send("DELETE FROM t WHERE id = 2")
    check("see own write", s.rows("SELECT name FROM t WHERE id = 1") == [["zz"]])
    check("rollback rows", len(s.rows("SELECT id FROM t")) == 1)
    s.send("ROLLBACK")
    check("after rollback", [r[0] for r in s.rows("SELECT id FROM t ORDER BY id")] == ["1", "2"])
    check("after rollback val", s.rows("SELECT name FROM t WHERE id = 1") == [["ab"]])

    # 事务：提交 + 重启
    s.send("BEGIN")
    s.send("INSERT INTO t VALUES (3,'ef','[0,0,1]')")
    s.send("COMMIT")
    s.close()
    s = Sess()
    check("commit survives restart", [r[0] for r in s.rows("SELECT id FROM t ORDER BY id")] == ["1", "2", "3"])
    check("varchar survives", s.rows("SELECT name FROM t WHERE id = 3") == [["ef"]])
    check("vector survives", s.rows("SELECT v FROM t WHERE id = 3") == [["[0,0,1]"]])

    # 未提交事务在重启后必须消失
    s.send("BEGIN")
    s.send("INSERT INTO t VALUES (4,'gh','[1,1,1]')")
    s.close()                            # 正常关闭：回滚未提交事务
    s = Sess()
    check("uncommitted gone", len(s.rows("SELECT id FROM t")) == 3)

    # 隐式提交：BEGIN 后 DDL
    s.send("BEGIN")
    s.send("INSERT INTO t VALUES (5,'ij','[1,1,0]')")
    s.send("CREATE TABLE t2 (id BIGINT PRIMARY KEY)")
    check("ddl implicit commit", len(s.rows("SELECT id FROM t")) == 4)
    check("t2 exists", len(s.rows("SHOW TABLES")) == 2)

    # 字符串比较：Unicode 码点序
    s.send("INSERT INTO t VALUES (6,'中','[1,0,1]'),(7,'z','[0,1,1]')")
    # UTF-8 字节序与码点序一致：'z' < '中'
    check("utf8 order", [r[0] for r in s.rows("SELECT id FROM t WHERE name >= 'z' ORDER BY id")] == ["6", "7"])
    check("utf8 order2", [r[0] for r in s.rows("SELECT id FROM t WHERE name < 'z' ORDER BY id")] == ["1", "2", "3", "5"])
    check("utf8 cp len", s.send("INSERT INTO t VALUES (8,'中文字符九宫格超出','[0,0,1]')")[1] == 1406)
    check("utf8 cp ok", s.send("INSERT INTO t VALUES (8,'中文字符九宫格超','[0,0,1]')")[0] == "ok")

    # 更新向量后检索结果随之变化
    s.send("UPDATE t SET v = '[1,0,0]' WHERE id = 3")
    r = s.rows("SELECT id, L2_DISTANCE(v,'[1,0,0]') AS d FROM t WHERE id = 3")
    check("update vector", r[0][0] == "3" and float(r[0][1]) < 1e-6, r)

    # 建索引不影响精确检索
    check("create vector index", s.send("CREATE VECTOR INDEX idx ON t(v)")[0] in ("ok",))
    r = s.rows("SELECT id, L2_DISTANCE(v,'[0,1,0]') AS d FROM t ORDER BY d LIMIT 3")
    check("topk after index", [x[0] for x in r] == ["2", "5", "7"], r)

    # LIMIT 与并列主键序
    s.send("DELETE FROM t")
    for i in range(1, 11):
        s.send("INSERT INTO t VALUES (%d,'same','[1,0,0]')" % i)
    r = s.rows("SELECT id FROM t ORDER BY id LIMIT 3")
    check("tie pk order", [x[0] for x in r] == ["1", "2", "3"], r)
    r = s.rows("SELECT id, L2_DISTANCE(v,'[1,0,0]') AS d FROM t ORDER BY d LIMIT 4")
    check("tie distance pk order", [x[0] for x in r] == ["1", "2", "3", "4"], r)

    # 过滤后 TopK：不足 K 返回全部
    s.send("INSERT INTO t VALUES (11,'other','[9,9,9]')")
    r = s.rows("SELECT id FROM t WHERE name = 'other' ORDER BY id")
    check("filter set", [x[0] for x in r] == ["11"], r)
    r = s.rows("SELECT id, L2_DISTANCE(v,'[1,0,0]') AS d FROM t WHERE name = 'other' ORDER BY d LIMIT 5")
    check("filter topk < k", [x[0] for x in r] == ["11"], r)

    # 新语法：COUNT / SELECT 常量 / TRUNCATE / IF EXISTS / LIMIT offset
    s.send("CREATE TABLE IF NOT EXISTS t (id BIGINT PRIMARY KEY)")
    check("if not exists", len(s.rows("SHOW TABLES")) == 2)
    check("count", s.rows("SELECT COUNT(*) FROM t")[0][0] == "11", s.rows("SELECT COUNT(*) FROM t"))
    check("count where", s.rows("SELECT COUNT(*) AS c FROM t WHERE name = 'other'")[0][0] == "1")
    check("select const", s.rows("SELECT 1") == [["1"]])
    check("select str const", s.rows("SELECT 'hi' AS s") == [["hi"]])
    s.send("BEGIN")
    s.send("TRUNCATE TABLE t2")
    r = s.rows("SELECT COUNT(*) FROM t2")
    check("truncate", r[0][0] == "0", r)
    s.send("ROLLBACK")
    check("drop if exists", s.send("DROP TABLE IF EXISTS nope")[0] == "ok")

    # 主键改写：唯一性校验 + 事务回滚 + 重启后不留旧行
    s.send("CREATE TABLE p (id BIGINT PRIMARY KEY, v VARCHAR(8))")
    s.send("INSERT INTO p VALUES (1,'a'),(2,'b')")
    s.send("UPDATE p SET id = 5 WHERE id = 1")
    check("pk update", s.rows("SELECT id FROM p ORDER BY id") == [["2"], ["5"]])
    check("pk dup", s.send("UPDATE p SET id = 2 WHERE id = 5")[1] == 1062)
    s.send("BEGIN")
    s.send("UPDATE p SET id = 7 WHERE id = 5")
    s.send("ROLLBACK")
    check("pk update rollback", s.rows("SELECT id FROM p ORDER BY id") == [["2"], ["5"]])
    s.send("BEGIN")
    s.send("UPDATE p SET id = 9 WHERE id = 5")
    s.send("COMMIT")
    s.close()
    s = Sess()
    check("pk update replays", [r[0] for r in s.rows("SELECT id FROM p ORDER BY id")] == ["2", "9"])
    check("pk old row gone", s.rows("SELECT COUNT(*) FROM p")[0][0] == "2")

    s.close()
    print("\n%d failed / total checks" % len(fails))
    return 0 if not fails else 1

if __name__ == "__main__":
    sys.exit(main())
