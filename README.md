# MiniVectorDB

MiniVectorDB 是本仓库 `2026-competition-round2` 分支的实现：单机、单进程、单客户端的轻量向量数据库。它通过 stdin 接收 SQL、stdout 返回结果，所有持久化文件位于 `--data-dir`（默认 `/data`），不监听网络端口。

## 1. 语言、构建与运行

- 语言：C++17（无第三方运行时依赖）
- 编译器：g++ 13+（Dockerfile 使用 `gcc:13` 多阶段构建）
- 平台：Linux x86_64
- 构建：

```bash
./build.sh
# 产出 build/minivectordb
```

Docker：

```bash
docker build -t minivectordb:latest .
docker run --rm -i -v minivectordb-data:/data \
  --cpus=2 --memory=2g minivectordb:latest
```

容器以前台方式运行，`ENTRYPOINT` 直接启动 `/app/minivectordb --data-dir /data`。评测驱动通过 stdin/stdout 管道交互；日志和诊断只写 stderr。

## 2. stdin/stdout 协议

默认协议（`--protocol framed`，也可用环境变量 `MVDB_PROTOCOL` 覆盖）：

启动完成后输出一行：

```
READY
```

之后每条 SQL 的响应：

- 查询（SELECT/SHOW/DESCRIBE）：

```
RESULT <列数> <行数>
<列名1>\t<列名2>...
<值1>\t<值2>...
...
```

- 非查询语句（CREATE/INSERT/UPDATE/DELETE/BEGIN/COMMIT/ROLLBACK/SET）：

```
OK <影响行数>
```

- 错误：

```
ERROR <错误码> <错误信息>
```

- 正常退出（EOF、`QUIT`、`EXIT`、`SHUTDOWN`、`\q`、SIGTERM、SIGINT）：

```
BYE
```

字段中的制表符、换行、反斜杠按 `\t`、`\n`、`\\` 转义。所有输出在每条语句后立即 flush。

为了兼容不同的评测封装，程序还提供两种可选输出模式（不影响默认协议）：

- `--protocol mysql` / `MVDB_PROTOCOL=mysql`：SELECT 输出表头和数据行；非查询输出 `OK`；错误输出 `ERROR <code> (<sqlstate>): <message>`。
- `--protocol tsv`（别名 `plain`）/ `MVDB_PROTOCOL=tsv`：只输出表头和数据行，非查询不输出。

可用环境变量微调协议，便于适配不同的评测封装：

- `MVDB_DML_STATUS=0`：非查询语句不输出 `OK`（framed/mysql 都适用）。
- `MVDB_READY=none` 或自定义字符串：关闭/替换启动就绪行。
- `MVDB_BYE=none` 或自定义字符串：关闭/替换退出行。

输入以分号 `;` 作为语句边界；也接受 mysql 客户端的 `\g` / `\G` 终止符。字符串、反引号标识符、`--`/`#`/`/* ... */` 注释中的分号不会切断语句。EOF 时缓冲区里最后一条未以分号结束的语句会执行。

### 精确 / 近似模式

`SET search_mode='ann'`、`SET search_mode='exact'`、`SET vector_search_mode=...`、`SET ann=1/0` 均可接受。实现始终返回精确 TopK（因此 ann 模式的 Recall@K 为 1.0）；`CREATE VECTOR INDEX` 的元数据会持久化，增删改后检索结果仍与当前可见记录一致。

## 3. SQL 支持范围

### DDL

```sql
CREATE TABLE [IF NOT EXISTS] t (
  id BIGINT PRIMARY KEY,
  s VARCHAR(32),
  v VECTOR(4)
);
CREATE VECTOR INDEX idx ON t(v);
DROP TABLE [IF EXISTS] t;
DROP INDEX idx ON t;
```

- 每表恰好一个 BIGINT 主键（列级 `PRIMARY KEY` 或表级 `PRIMARY KEY (col)`）。
- 每表最多一个 VECTOR 列，维度固定。
- VARCHAR 长度按 Unicode 码点数校验；字符串比较按 Unicode 码点顺序。
- 不支持 NULL、JOIN、聚合、子查询、ALTER TABLE、外键、嵌套事务、主键更新；这些语句返回明确错误，不会静默执行。

### DML

```sql
INSERT INTO t [(c1, c2, ...)] VALUES (v1, v2, ...), (...);
UPDATE t SET c = expr [, ...] [WHERE cond];
DELETE FROM t [WHERE cond];
```

- 多行 INSERT 是原子的：任意一行失败则整条语句不生效。
- 主键重复返回 1062；VARCHAR 超长返回 1406；BIGINT/float32 溢出返回 1264/1366。
- UPDATE 不能修改主键。

### 查询

```sql
SELECT * | expr [AS alias] [, ...]
FROM t
[WHERE cond]
[ORDER BY expr [ASC|DESC] [, ...]]
[LIMIT n [OFFSET m] | LIMIT m, n];
```

- WHERE 支持 `=`、`!=`、`<>`、`<`、`<=`、`>`、`>=`、`AND`、`OR`、`NOT` 和括号。
- ORDER BY 的并列项按主键升序；向量 TopK 的距离并列也按主键升序。
- 未指定 ORDER BY 时按主键升序输出，便于确定性比较（题目允许按集合比较）。
- `L2_DISTANCE(vector_col, '[q1,q2,...]')` 返回欧氏距离；查询向量维度必须与列一致。
- 有 WHERE 时先在满足条件的集合中过滤，再计算距离和 TopK，不会先全表取 K 再过滤。

### 事务

```sql
BEGIN;      -- 或 START TRANSACTION
COMMIT;
ROLLBACK;
```

- 同一时刻最多一个显式事务。
- 事务内查询/向量检索能看到本事务已完成的修改。
- 未显式 BEGIN 时，成功的 DML 自动提交。
- 语句失败不会留下部分修改；之前成功的语句仍可提交或回滚。
- 正常关闭（EOF/SIGTERM/SIGINT）回滚未提交事务并 checkpoint。
- DDL（CREATE/DROP）在显式事务内按 MySQL 语义隐式提交当前事务；不支持真正的嵌套事务。

### 错误码

使用 MySQL 风格错误码，常见值：

| 错误码 | 含义 |
| ---: | --- |
| 1064 | 语法错误 |
| 1050 | 表已存在 |
| 1146 | 表不存在 |
| 1054 | 列不存在 |
| 1062 | 主键重复 |
| 1136 | 列数不匹配 |
| 1406 | VARCHAR 超长 |
| 1264 | 整数越界 |
| 1366 | 向量格式/维度/float32 范围非法 |
| 1235 | 不支持的语句/功能 |
| 1568 | 事务状态错误 |

## 4. 从空目录开始的示例

```bash
rm -rf /tmp/mvdb && mkdir -p /tmp/mvdb
./build/minivectordb --data-dir /tmp/mvdb
```

输入：

```sql
CREATE TABLE docs (
    id BIGINT PRIMARY KEY,
    category VARCHAR(32),
    content VARCHAR(1024),
    embedding VECTOR(4)
);
INSERT INTO docs VALUES
    (1, 'database', '事务与恢复', '[1, 0, 0, 0]'),
    (2, 'database', '向量索引',   '[0, 1, 0, 0]'),
    (3, 'travel',   '旅行攻略',   '[0, 0, 1, 0]');
SELECT id, content,
       L2_DISTANCE(embedding, '[0.9, 0.1, 0, 0]') AS distance
FROM docs
WHERE category = 'database'
ORDER BY distance
LIMIT 2;
BEGIN;
UPDATE docs SET embedding = '[0.8, 0.2, 0, 0]' WHERE id = 1;
DELETE FROM docs WHERE id = 2;
ROLLBACK;
SELECT id, content FROM docs ORDER BY id;
```

输出（节选）：

```
READY
OK 0
OK 3
RESULT 3 2
id	content	distance
1	事务与恢复	0.141421374149721
2	向量索引	1.272792188223375
OK 0
OK 1
OK 1
OK 0
RESULT 2 2
id	content
1	事务与恢复
3	旅行攻略
BYE
```

重启复用 `/tmp/mvdb` 即可看到 ROLLBACK 后的数据；`/data` 下会生成 `catalog.db`、`meta.db`、`wal.log`、`tbl_<id>.dat`。

## 5. 设计与实现

### 记录布局

每个表一个 append-only 数据文件 `tbl_<table_id>.dat`。记录头 36 字节：

```
u32 magic | u8 kind | u8 flags | u16 reserved |
u64 txn_id | u64 row_id | u32 table_id |
u32 payload_len | u32 crc32 | payload
```

`kind=ROW` 的 payload 按列顺序编码：

- BIGINT：8 字节小端；
- VARCHAR：u32 字节长度 + UTF-8 字节；
- VECTOR：`dim × float32` 小端。

`kind=DEL` 的 payload 为空，`row_id` 表示删除。CRC32 覆盖记录头（crc 字段置零）和 payload。启动时顺序扫描、校验 CRC；尾部不完整记录会被截断。

内存索引为 `row_id -> (offset, payload_len)` 的哈希表；另有一个受预算限制的行缓存（默认 32 MiB，`MVDB_CACHE_MB` 可调）。缓存足够时，查询走连续快照；数据超过缓存时，按 offset 从 mmap 流式读取，不会全量载入内存。

### WAL 与提交

`wal.log` 使用同样的 36 字节记录头，类型为 `ROW`、`DEL`、`COMMIT`、`ABORT`。

提交顺序：

1. 把本事务的全部 ROW/DEL 记录和 COMMIT 追加到 WAL；
2. `fdatasync` WAL；
3. 把同样的记录追加到各表数据文件（不 fsync）；
4. 更新内存索引和缓存；
5. 返回成功。

因此成功响应前唯一必须落盘的是 WAL；数据文件可以由 WAL 重放补齐。批量写入每批 100 行只产生一次 WAL fsync。

### 恢复

启动时：

1. 读取 `catalog.db` 与 `meta.db`（checkpoint txn）；
2. 扫描 WAL，得到已提交事务集合和最大 txn；
3. 扫描每个表数据文件，只保留 `txn <= checkpoint` 或已提交事务的记录，CRC/尾部不完整的记录截断；
4. 重放 WAL 中已提交的 ROW/DEL（可能产生重复记录，语义上后写覆盖）；
5. 对需要清理的表做 compaction（只写当前可见行）；
6. 保存更大的 checkpoint，截断 WAL，之后才报告 `READY`。

恢复过程是幂等的：在恢复途中再次被 SIGKILL，下次启动仍从 WAL 和数据文件重新得到同样的可见集合。已返回 COMMIT 的事务不会丢；未提交事务的记录不会生效。

周期性 checkpoint：WAL 超过 64 MiB 时，在自动提交后 fsync 所有数据文件、保存 checkpoint、截断 WAL，避免 WAL 无限增长。

### 事务与可见性

显式事务的修改先进入内存 overlay（`row_id -> optional<Row>`），不写数据文件；查询/更新/删除/向量扫描先看 overlay，再读基础行。COMMIT 时把 overlay 的最终状态编码进 WAL/数据文件；ROLLBACK 直接丢弃 overlay。这样语句失败、回滚都不会留下部分修改。

### 向量检索

- 精确 L2 扫描；AVX2/FMA 加速（运行时检测，带标量回退），使用双精度累加。
- 对 `ORDER BY L2_DISTANCE(...) LIMIT k` 做了快速路径：只维护大小为 `offset+limit` 的堆，避免为每一行构造结果对象。
- 过滤在距离计算之前执行；带过滤的 TopK 严格在满足 WHERE 的集合内计算。
- `CREATE VECTOR INDEX` 只保存索引元数据；ann 模式仍返回精确结果，因此召回率不会低于要求。

### 内存与磁盘

- 行缓存默认 32 MiB，可用 `MVDB_CACHE_MB` 调整。
- 数据文件用 mmap 读取；大表按记录流式处理。
- 所有数据库文件都在 `--data-dir` 下，不写其他目录。
- 程序不启动线程、不监听端口、不访问网络。

## 6. 本地验证

`tests/selftest.py` 覆盖：建表/增删改查、类型与错误、事务提交/回滚、持久化重启、强制终止恢复、重复恢复、精确 TopK 与过滤 TopK、`CREATE VECTOR INDEX`、`ann` 模式。

`tests/run_public_cases.py` 直接读取官方公开题包 `mysqltest/manifest.json` 与 `cases/*.test|*.result`，按 `--restart` / `--mode ann` 驱动指令逐条执行并比较 JSON 断言（`{}`、`error`、`affected_rows`、`columns`、`rows`，浮点容差 1e-5）。

```bash
./build.sh
python3 tests/selftest.py
python3 tests/run_public_cases.py
```

已验证的记录（本机 WSL Ubuntu，g++ 15，2026-11-09）：

- 官方公开题包 20/20 用例通过（95 条 SQL，包含 BIGINT 精度、float32、过滤后 TopK、事务原子性、正常关闭回滚、`--restart`、`--mode ann`）；
- GitHub Actions `.github/workflows/minivectordb-ci.yml` 在干净的 Ubuntu runner 上 `docker build` 成功，并用 `docker run --rm -i` 跑通建表/插入/事务回滚/重启后向量查询；
- 10,000 行 × 128 维，100 行/批写入：约 0.36 s；1,000 次 TopK（一半带过滤）：约 0.70 s；
- 100,000 行 × 128 维（数据文件约 106 MiB，含 WAL），缓存限制 1 MiB：查询约 0.11 s，进程峰值 RSS 约 65 MiB；
- 随机 20,000 行 × 64 维，与按存储后 float32 值计算的暴力 Top10 对比：ID 全部一致，距离误差 < 1e-7；
- SIGKILL 后重启：已提交行保留，未提交事务的 INSERT/DELETE/UPDATE 不生效；恢复途中连续 5 次 SIGKILL 后仍可正常启动。

## 7. 已知限制

- 只支持题目要求的 SQL 子集；JOIN、聚合、子查询、NULL、ALTER、嵌套事务、主键更新会返回错误。
- 单客户端、单事务；没有多客户端并发控制。
- ann 模式返回精确结果，没有实现近似索引结构；如果评测对 ann 查询的耗时提出额外要求，需要另加索引。
- 每表最多一个 VECTOR 列；VARCHAR 不做语言相关排序（按 UTF-8 码点）。
- 输入单条语句上限 64 MiB；超长语句返回错误。
- 进程被 SIGKILL 后的恢复会重放 WAL 并可能重写数据文件；恢复时间与数据量成正比。
- 没有实现磁盘损坏/断电语义；崩溃模型限定为进程被强制终止。

## 8. Agent Prompt 学习材料

见仓库根目录 `conversation.json`（同一份材料也放在 `agent/conversation.json`）。材料只包含本次开发中的真实 prompt、工具输出、实验记录和复盘，未编造未执行的实验。
