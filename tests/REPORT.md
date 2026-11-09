# 本地验证记录（2026-11-09）

环境：4 核 / 23 GiB / Ubuntu，gcc 11.4，CPU 支持 AVX2+FMA（无 AVX-512，运行时判定为 level 2）。
全部命令在仓库根目录执行，数据目录用仓库内 `.xxxdb`（已被 .gitignore 忽略）。

| # | 命令 | 结果 |
|---|---|---|
| 1 | `python3 tests/sql_suite.py` | `0 failed / total checks` |
| 2 | `python3 tests/topk_check.py` | `checked 12 queries, mismatch=0` |
| 3 | `python3 tests/crash_test.py` | `committed batches=44 expect=4400 got=4400 contiguous=True` / `after torn-tail got=4400 same=True` |
| 4 | `python3 tests/bench.py` | wall `insert 1.0–1.9 s` / `query 0.03–0.26 ms`；**CPU 口径（更稳）** `insert 380 ms` / `query 40–60 ms per 1000` / `peakRSS 17.4 MiB` / `wal 5.31 MiB` |
| 5 | `python3 tests/validate_agent_json.py agent/conversation.json` | `size=16.0 KiB, units=3` / `OK` |

## 覆盖点

- **语义**：CREATE/DROP/DESC/SHOW TABLES；INSERT 多行与重复主键(1062)、列数不符(1136)、超长(1406)、类型不符(1366)、
  向量维度不符与 NaN/Inf(1366)；WHERE 六种比较 + AND；ORDER BY 列/别名/序号 + LIMIT；并列按主键升序；
  过滤后 TopK 在过滤集合内计算且不足 K 返回全部。
- **事务**：BEGIN 内可见自身修改；ROLLBACK 恢复；COMMIT 后跨重启可见；未提交事务在正常关闭时回滚；
  DDL 前隐式提交。
- **恢复**：提交过程中 SIGKILL 只丢未提交事务；WAL 尾部撕裂记录被截断；重启后 VARCHAR/VECTOR 均完整。
- **检索正确性**：输入按 float32 量化后与 float64 暴力解比对 id 序列，含并列与过滤。
- **性能**：题面 default 负载（10k 行 / 128 维 / 1k 次 TopK / K=10 / 半带过滤 / 每批 100 行提交）。

## 本轮修掉的真实缺陷（都有复现与回归）

1. **字典码落盘**：WAL 写表内字典码 → 重启后 VARCHAR 全空；改为按内容落盘，重放时重建字典。
2. **投影下标越界**：`L2_DISTANCE` 投影时 `qvec` 只对距离项 push，用投影下标索引即越界（SIGSEGV）。
3. **UPDATE 误判**：`where_match` 把"不匹配"与"求值出错"混在一个 bool，未命中行报 `ERROR 0`。
4. **主键改写重放不完整**：`UPDATE SET id=5` 后旧行在重启后复活（WAL 只记了新行）→ 拆成"删旧主键 + 插新行"。
5. **协议缺终止符**：驱动读到结果后无法判断结束，崩溃测试挂起 → 输出加可配置终止符。

## 已知未验证项

- 与官方测试驱动的协议对接（题包未公开）：三套输出格式与终止符均可用环境变量切换。
- Dockerfile 本地未构建（本机无 docker/podman）。
- 距离文本格式未与官方预期结果对齐（默认最短往返，可 `MVDB_FLOAT_FMT` 切换）。
