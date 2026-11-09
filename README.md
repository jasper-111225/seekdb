# MiniVectorDB

单机轻量数据库：基础 SQL + 向量检索 + 持久化 + 事务 + 崩溃恢复。
C++17，零第三方依赖（仅 C++ 标准库 + POSIX），单进程、单线程，stdin/stdout 交互。

## 构建与运行

```bash
make -j"$(nproc)"            # 产物 ./minivectordb
docker build -t minivectordb:latest .
docker run --rm -i -v minivectordb-data:/data --cpus=2 --memory=2g minivectordb:latest
```

启动参数：`--data-dir DIR`（默认 `/data`）、`--proto mysql|block|jsonl`（默认 `mysql`）、
`--mode exact|ann`（默认 `exact`；两者都走精确检索）。日志写 stderr，stdout 只放结果。

## SQL 支持范围

- DDL：`CREATE TABLE`（`BIGINT` / `VARCHAR(n)` / `VECTOR(dim)`，列级或表级 `PRIMARY KEY`）、`DROP TABLE`、
  `CREATE [VECTOR] INDEX name ON t(col)`（索引可选，检索恒精确）、`SHOW TABLES`、`DESC t`。
- DML：`INSERT`（多行、可选列名）、`UPDATE`、`DELETE`，均支持 `WHERE`（`=` `<>` `<` `<=` `>` `>=`、`AND`）。
- 查询：`SELECT *` / 列 / `L2_DISTANCE(col,'[...]') AS d`，`WHERE`、`ORDER BY`（列名、别名、序号、`ASC/DESC`）、`LIMIT`。
- 事务：`BEGIN` / `COMMIT` / `ROLLBACK`，自动提交，DDL 前隐式提交。
- 其他：`EXIT` / `QUIT`；`--`、`#`、`/* */` 注释；字符串支持 `''` 与 `\'` 转义。

语义要点：VARCHAR 按 Unicode 码点计长与比较（UTF-8 字节序即码点序）；VECTOR 为定维 float32，
拒绝 NaN/Inf/维度不符；过滤后的 TopK 在过滤集合内计算；距离相同按主键升序；`exact` 模式恒返回精确结果。

## 持久化与恢复

- `/data/wal.log`：提交日志，记录粒度为一个事务；每条 `[magic][crc32c][len][payload]`。
- `/data/base.dat`：checkpoint（全量快照），写 tmp → fsync → rename → fsync(dir)，随后截断 WAL。
- 启动时先读 base 再顺序重放 WAL；遇到撕裂/损坏尾部即截断到最后一个完整记录。
- 提交 = 追加记录 + `fdatasync`；进程被杀只丢未提交事务。

## 官方公开题包自测

官方 `2026-competition-round2` 分支上的 `mysqltest/` 是 20 个公开样例（95 条 SQL）。本仓库的
`tests/run_public.py` 按 `mysqltest/README.md` 的断言模型复刻了本地驱动：

```bash
python3 tests/run_public.py      # 20/20 cases passed
```

断言模型：每个 `.test` 非空行一条 SQL，`--restart` 表示正常关闭后用**同一数据目录**重启（未提交事务回滚），
`--mode ann` 只影响之后的向量查询；同名 `.result` 与 SQL 顺序一一对应，条目为 `{}`、`{"error": "duplicate_key"}`、
`{"affected_rows": n}` 或 `{"columns": [...], "rows": [[...]], "unordered": true}`。浮点容差 1e-5，
整数/字符串精确比较，`unordered` 用多重集比较（重复行不可丢）。

错误行同时给出数字码与语义名，便于驱动二选一匹配：

```
ERROR 1062 (23000) [duplicate_key]: Duplicate entry '1' for key 'PRIMARY'
```

## 本地验证

```bash
python3 tests/sql_suite.py     # SQL 语义 / 事务 / 重启恢复 / 边界与错误码
python3 tests/crash_test.py    # 提交过程中 SIGKILL，重启校验
python3 tests/topk_check.py    # TopK 与暴力解差分（float32 量化后）
python3 tests/bench.py         # 10k 行 / 128 维 / 1000 次 TopK 的耗时与内存
```

## 已知限制

- 单线程模型；百万级以上建议加索引或分段。
- `ann` 模式当前与 `exact` 结果一致（未做近似剪枝）。
- 未实现 `COUNT/SUM` 等聚合、`JOIN`、二级索引。
