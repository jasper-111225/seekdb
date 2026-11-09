#!/usr/bin/env python3
"""提交物自检：在 push 之前挡住"文件在树里但内容是空/缺失"这类致命问题。

背景：官方分支上的 Dockerfile 和 conversation.json 是 0 字节的占位符，
改完代码后如果只 checkout 了 src/ 等路径，Dockerfile 会保持 0 字节，
平台构建镜像时直接失败（表现为"执行失败"）。这个脚本把这类检查固化下来。
"""
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
problems = []
notes = []


def tracked(path):
    """git 树里该路径的内容（None 表示未跟踪）。"""
    r = subprocess.run(["git", "-C", ROOT, "show", "HEAD:" + path],
                       capture_output=True)
    return r.stdout if r.returncode == 0 else None


def check_dockerfile():
    blob = tracked("Dockerfile")
    if blob is None:
        problems.append("Dockerfile 未被 git 跟踪")
        return
    text = blob.decode("utf-8", "replace")
    if len(blob) == 0:
        problems.append("Dockerfile 是 0 字节（官方占位符没被覆盖）")
        return
    for needle in ("COPY src", "COPY Makefile", "ENTRYPOINT", "ubuntu:24.04"):
        if needle not in text:
            problems.append("Dockerfile 缺少 %r" % needle)
    if "apt-get install" not in text and "make" in text:
        problems.append("Dockerfile 构建阶段没有安装 make/g++")
    notes.append("Dockerfile %d 字节" % len(blob))


def check_makefile():
    blob = tracked("Makefile")
    if blob is None:
        problems.append("Makefile 未被 git 跟踪（上游 .gitignore 有裸 Makefile 规则）")
        return
    text = blob.decode("utf-8", "replace")
    for needle in ("$(wildcard src/*.cpp)", "minivectordb"):
        if needle not in text:
            problems.append("Makefile 缺少 %r" % needle)
    notes.append("Makefile %d 字节" % len(blob))


def check_sources():
    out = subprocess.run(["git", "-C", ROOT, "ls-tree", "-r", "HEAD", "--name-only"],
                         capture_output=True, text=True).stdout.split()
    cpp = [p for p in out if p.startswith("src/") and p.endswith((".cpp", ".h"))]
    if len(cpp) < 5:
        problems.append("src/ 下被跟踪的源文件太少：%s" % cpp)
    for p in cpp:
        blob = tracked(p)
        if not blob:
            problems.append("%s 是空文件" % p)
    notes.append("src 文件 %d 个" % len(cpp))


def check_conversation():
    root = os.path.join(ROOT, "conversation.json")
    agent = os.path.join(ROOT, "agent", "conversation.json")
    for path in (root, agent):
        if not os.path.exists(path):
            problems.append("缺少 %s" % os.path.relpath(path, ROOT))
            continue
        raw = open(path, "rb").read()
        if len(raw) == 0:
            problems.append("%s 是 0 字节" % os.path.relpath(path, ROOT))
            continue
        if len(raw) > 128 * 1024:
            problems.append("%s 超过 128KiB" % os.path.relpath(path, ROOT))
        try:
            data = json.loads(raw.decode("utf-8"))
        except Exception as exc:  # noqa: BLE001
            problems.append("%s 不是合法 JSON: %s" % (os.path.relpath(path, ROOT), exc))
            continue
        for key in ("meta", "user_prompts", "conversations", "experiments", "units"):
            if key not in data:
                problems.append("%s 缺少 %r" % (os.path.relpath(path, ROOT), key))
    if os.path.exists(root) and os.path.exists(agent):
        if open(root, "rb").read() != open(agent, "rb").read():
            problems.append("根 conversation.json 与 agent/conversation.json 内容不一致")
    notes.append("conversation.json %d 字节" % os.path.getsize(root))


def check_tests_package():
    for p in ("mysqltest/manifest.json", "mysqltest/README.md"):
        if tracked(p) is None and not os.path.exists(os.path.join(ROOT, p)):
            problems.append("缺少 %s（官方公开题包）" % p)


def check_no_secrets():
    out = subprocess.run(["git", "-C", ROOT, "grep", "-l", "-E", "ghp_[A-Za-z0-9]{20,}"],
                         capture_output=True, text=True).stdout.split()
    if out:
        problems.append("提交里出现疑似 token：%s" % out)


def check_no_data_dirs():
    out = subprocess.run(["git", "-C", ROOT, "ls-tree", "-r", "HEAD", "--name-only"],
                         capture_output=True, text=True).stdout.split()
    junk = [p for p in out if p.startswith((".ckptdb", ".publicdb", ".benchdb", ".crashdb",
                                            ".tmpdb", ".suitdb", ".topkdb", ".qdb",
                                            ".q2/", ".q3/", ".dbg", ".e1/", ".e2/", ".e3/"))]
    if junk:
        problems.append("提交里混入测试数据文件：%s" % junk[:5])


def main():
    for fn in (check_dockerfile, check_makefile, check_sources, check_conversation,
               check_tests_package, check_no_secrets, check_no_data_dirs):
        fn()
    for n in notes:
        print("ok   " + n)
    if problems:
        for p in problems:
            print("FAIL " + p)
        print("%d problem(s)" % len(problems))
        return 1
    print("submission OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
