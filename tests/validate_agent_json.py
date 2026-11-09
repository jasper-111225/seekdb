#!/usr/bin/env python3
"""校验 conversation.json：大小、UTF-8、重复键、RFC6901 可定位性、原文匹配、证据去重。"""
import json, sys

MAX = 128 * 1024

def dup_hook(pairs):
    seen = {}
    for k, v in pairs:
        if k in seen:
            raise ValueError("duplicate key: %r" % k)
        seen[k] = v
    return seen

def resolve(doc, pointer):
    if pointer == "":
        return doc
    cur = doc
    for tok in pointer.split("/")[1:]:
        tok = tok.replace("~1", "/").replace("~0", "~")
        if isinstance(cur, list):
            cur = cur[int(tok)]
        else:
            cur = cur[tok]
    return cur

def main(path):
    raw = open(path, "rb").read()
    errs = []
    if len(raw) > MAX:
        errs.append("size %d > %d" % (len(raw), MAX))
    try:
        text = raw.decode("utf-8")
    except UnicodeDecodeError as e:
        print("not utf-8:", e); return 1
    if "NaN" in text or "Infinity" in text:
        errs.append("contains NaN/Infinity token")
    doc = json.loads(text, object_pairs_hook=dup_hook)
    if not isinstance(doc, dict):
        errs.append("top-level not object")
    seen_quotes = {}
    for i, u in enumerate(doc.get("units", [])):
        for side in ("student_evidence", "result_evidence"):
            ev = u.get(side)
            if not ev:
                continue
            try:
                target = resolve(doc, ev["pointer"])
            except Exception as e:
                errs.append("%s %s pointer unresolvable %r: %s" % (u.get("id"), side, ev["pointer"], e))
                continue
            if not isinstance(target, str):
                errs.append("%s %s target not string" % (u.get("id"), side))
                continue
            q = ev.get("quote", "")
            if len(q) > 240:
                errs.append("%s %s quote %d chars > 240" % (u.get("id"), side, len(q)))
            if q not in target:
                errs.append("%s %s quote not substring of %s" % (u.get("id"), side, ev["pointer"]))
            if q in seen_quotes:
                errs.append("%s %s quote reused from %s" % (u.get("id"), side, seen_quotes[q]))
            seen_quotes[q] = "%s/%s" % (u.get("id"), side)
        se, re_ = u.get("student_evidence"), u.get("result_evidence")
        if se and re_ and se.get("quote") == re_.get("quote"):
            errs.append("%s student/result evidence identical" % u.get("id"))
        t = u.get("tier")
        if t == 0.5 and re_:
            errs.append("%s tier 0.5 must not carry result_evidence" % u.get("id"))
        if t and t > 0.5 and not re_:
            errs.append("%s tier >0.5 requires result_evidence" % u.get("id"))
    print("size=%d bytes (%.1f KiB), units=%d" % (len(raw), len(raw) / 1024.0, len(doc.get("units", []))))
    for e in errs:
        print("ERROR", e)
    print("OK" if not errs else "%d problem(s)" % len(errs))
    return 0 if not errs else 1

if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "agent/conversation.json"))
