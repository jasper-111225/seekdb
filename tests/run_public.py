#!/usr/bin/env python3
"""跑公开题包 mysqltest/cases（20 个样例 / 95 条 SQL），按其 README 的断言模型比对。

- 每行一条 SQL；`--restart` / `--mode ann` 是驱动指令，不占断言位置
- .result 是 JSON 数组：{} / {"error": name} / {"affected_rows": n} / {"columns": [...], "rows": [[...]], "unordered": bool}
- 浮点容差 1e-5；整数与字符串精确
"""
import json, os, re, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(ROOT)
BIN = os.path.join(REPO, "minivectordb")
CASES = os.path.join(REPO, "mysqltest", "cases")
DB = os.path.join(REPO, ".publicdb")

LAYER = {
    1062: "duplicate_key",
    1366: "dimension_mismatch",
    1406: "value_too_long",
    1146: "unknown_table",
    1054: "unknown_column",
    1235: "unsupported_sql",
    1064: "unsupported_sql",
}

class Session:
    def __init__(self, mode="exact"):
        self.p = subprocess.Popen([BIN, "--data-dir", DB, "--mode", mode], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1 << 16)
        self.p.stdout.readline()

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
            code = int(m.group(1)) if m else 0
            return {"error": LAYER.get(code, "error_%d" % code), "raw": lines[0], "code": code, "msg": lines[0]}
        if lines and lines[0].startswith("Query OK"):
            m = re.search(r"Query OK, (\d+) row", lines[0])
            return {"affected_rows": int(m.group(1)) if m else 0}
        if not lines:
            return {"affected_rows": 0}
        return {"columns": lines[0].split("\t"), "rows": [l.split("\t") for l in lines[1:]]}

    def close(self):
        try:
            self.p.stdin.close()
            self.p.wait(timeout=30)
        except Exception:
            self.p.kill()

def num_eq(got, exp):
    try:
        if isinstance(exp, bool):
            return False
        if isinstance(exp, int):
            return int(got) == exp
        return abs(float(got) - float(exp)) <= 1e-5 * max(1.0, abs(float(exp)))
    except Exception:
        return False

def cmp_row(got, exp):
    if len(got) != len(exp):
        return False
    for g, e in zip(got, exp):
        if isinstance(e, (int, float)):
            if not num_eq(g, e):
                return False
        else:
            if g != str(e):
                return False
    return True

def check(got, exp):
    if "error" in exp:
        if "error" not in got:
            return False, "expected error %s, got %s" % (exp["error"], got)
        name = exp["error"]
        return (got["error"] == name or name in got.get("msg", "")), "error name %s vs %s" % (name, got["error"])
    if "error" in got:
        return False, "unexpected error: %s" % got["raw"]
    if "affected_rows" in exp:
        ok = got.get("affected_rows") == exp["affected_rows"]
        return ok, "affected %s vs %s" % (exp["affected_rows"], got.get("affected_rows"))
    if "columns" in exp:
        if got.get("columns") != exp["columns"]:
            return False, "columns %s vs %s" % (exp["columns"], got.get("columns"))
        gr, er = got["rows"], exp["rows"]
        if exp.get("unordered"):
            gs = [tuple(r) for r in gr]
            n = len(er)
            used = [False] * n
            for row in er:
                for i, other in enumerate(gs):
                    if not used[i] and cmp_row(other, row):
                        used[i] = True
                        break
        ok = len(gr) == len(er) and all(cmp_row(g, e) for g, e in zip(gr, er))
        return ok, "rows %s vs %s" % (er, gr)
    return True, ""

def run_case(cid, tpath, rpath):
    with open(tpath, encoding="utf-8") as f:
        lines = [l.strip() for l in f if l.strip()]
    with open(rpath, encoding="utf-8") as f:
        expect = json.load(f)
    shutil.rmtree(DB, ignore_errors=True)
    s = Session()
    ei = 0
    k = 0
    fails = []
    mode = "exact"
    for line in lines:
        if line.startswith("--restart") or line == "--restart":
            s.close()
            s = Session(mode)
            continue
        if line.startswith("--mode"):
            mode = line.split()[-1]
            continue
        if ei >= len(expect):
            fails.append("extra sql #%d: %s" % (ei, line))
            break
        got = s.send(line)
        ok, why = check(got, expect[ei])
        if not ok:
            fails.append("  [%d] %s\n      %s" % (ei, line[:110], why))
        ei += 1
        k += 1
    if ei != len(expect):
        fails.append("assert count mismatch: ran %d, expected %d" % (ei, len(expect)))
    s.close()
    return fails, k

def main():
    with open(os.path.join(REPO, "mysqltest", "manifest.json"), encoding="utf-8") as f:
        man = json.load(f)
    total_fail = 0
    only = sys.argv[1] if len(sys.argv) > 1 else None
    for c in man["cases"]:
        if only and only not in c["id"]:
            continue
        fails, n = run_case(c["id"], os.path.join(REPO, "mysqltest", c["test"]),
                            os.path.join(REPO, "mysqltest", c["result"]))
        status = "OK  " if not fails else "FAIL"
        print("%s %-32s %2d stmts" % (status, c["id"], n))
        if fails:
            total_fail += 1
            for f in fails:
                print(f)
    print("\n%d/%d cases passed" % (len(man["cases"]) - total_fail, len(man["cases"])))
    return 0 if total_fail == 0 else 1

if __name__ == "__main__":
    sys.exit(main())
