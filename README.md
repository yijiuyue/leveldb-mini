# mini LevelDB

从零实现的 LSM 存储引擎（C++17），约 2400 行代码，14000+ 项测试。

一个可以用 `Put / Get / Delete` 的键值数据库：写入先落 WAL，内存用跳表维护有序数据，
攒满后落成带稀疏索引的 SSTable，崩溃后靠重放 WAL 恢复。

## 特性

| 模块 | 实现 |
|---|---|
| **WAL** | 32KB block，`[crc32][len][payload]`，CRC 覆盖 length+data；读到坏记录**静默截断** |
| **MemTable** | 跳表（12 层，1/4 概率上升）；写单线程、**读无锁**（`release`/`acquire`） |
| **SSTable** | 4KB data block（**前缀压缩 + restart 索引**）+ index block + 40 字节 footer |
| **稀疏索引** | 每个 block 一条；实测占 **0.74%**，可全量加载到内存 |
| **布隆过滤器** | 整文件一个，双哈希，10 bit/key；实测**假阴性 0、假阳性 0.73%** |
| **墓碑删除** | `[type][payload]` 编码，`kDeleted` 命中即停止（防止旧值复活） |
| **崩溃恢复** | 重放所有 WAL（幂等）→ 落盘 → 删旧 WAL，保证「数据要么在 SSTable，要么在唯一的 WAL」 |

## 实测效果

**前缀压缩**（10000 条共享前缀的 key）：

```
压缩后  79 字节 / 不压缩 304 字节  = 省 74%
```

**稀疏索引**（20000 条 / 309KB）：

```
76 个 data block（平均 4069 字节，目标 4096）
索引占 0.74%（2.3KB）→ 整个索引能放内存
一次 Get 只需 1 次 pread（读 1 个 data block）
```

**布隆过滤器**（10000 个 key）：

| bits/key | k | 过滤器大小 | 假阳性率 |
|---|---|---|---|
| 4 | 2 | 2501 字节 | 15.56% |
| 6 | 4 | 3751 字节 | 5.44% |
| **10（默认）** | **6** | **6251 字节** | **0.60%** |
| 14 | 9 | 8751 字节 | 0.06% |

**假阴性始终为 0**——存在的 key 一定命中。

## 快速开始

```bash
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j4

# 手动试
./db_demo /tmp/mydb put name alice
./db_demo /tmp/mydb get name        # alice
./db_demo /tmp/mydb del name
./db_demo /tmp/mydb get name        # (not found)
./db_demo /tmp/mydb fill 10000      # 批量写
./db_demo /tmp/mydb stat

# 压测
./benchmark /tmp/ldb_bench 100000
```

## 测试

```bash
./run_tests.sh                      # 14000+ 项单元测试
./tests/run_crash_test.sh 100       # 100 轮随机 kill -9
```

### kill -9 崩溃恢复测试

每轮：启动 writer 不停 `Put(sync=true)`（每条确认后打印序号）→ 随机等 20–220ms →
`kill -9` → 重新打开验证。

**100 轮全部通过**，且每轮 `recovered = acked + 1`
（最后一条已 fsync 落盘、但还没来得及打印 ack）。

**这验证了核心语义**：`Put(sync=true)` 返回 OK 的数据，进程被 kill -9 也不丢。

## 实测性能

环境：VMware 虚拟机，Linux，Release 构建，10 万条数据。

```
写入（sync=false）       100000 次   0.118 秒    848979 次/秒
写入（sync=true）          2000 次   0.949 秒      2107 次/秒
随机读（存在的 key）     100000 次   4.105 秒     24362 次/秒
读不存在的 key（布隆）   100000 次   3.965 秒     25223 次/秒

磁盘占用：1.78 MB / 100000 条 = 22.3 字节/条
```

**关于 `sync` 的差距（849k vs 2.1k）**：

`sync=false` 只写进 OS 页缓存，`sync=true` 每条都 `fsync`。
408 倍差距就是 fsync 的真实代价（每条约 0.47ms）。

**关于布隆过滤器几乎没提升（25223 vs 24362）**：

布隆确实省掉了 data block 的 `pread`，但每次 `Get` 都要对每个 SSTable 调 `Table::Open`
（open + lseek + 读 footer + 读 index + 解析整个 index + close ≈ 40μs），
**这部分开销盖过了省下的那次 pread**。

真正的修法是 `TableCache`（缓存已打开的 Table，索引常驻内存）。加上之后：
- 存在的 key → 1 次 pread
- 不存在的 key → **0 次 pread**（布隆直接返回）

**这条限制记录在下方「已知限制」里。**

## 设计文档

- [整体架构：一条数据的完整路径](docs/00-architecture.md) —— Put / Get / 恢复三条路径 + 模块因果链
- [SSTable 字节级格式](docs/01-sstable-format.md) —— block / index / footer 的完整设计
- [数据结构速查](docs/03-data-structures.md) —— 每个结构的字节布局 + 实测数字
- [名词表](docs/02-glossary.md) —— 21 个专有名词

## 已知限制

| # | 限制 | 后果 | 修法 |
|---|---|---|---|
| 1 | **没有 compaction** | SSTable 只增不减，读放大 O(文件数) | 合并 + 清理墓碑与旧版本 |
| 2 | 每次 Get 都 open/close Table | 每次 Get 多约 40μs | TableCache（LRU） |
| 3 | 读也加锁 | 并发读性能差 | 引用计数 + 无锁读（SSTable 不可变） |
| 4 | 没有 MANIFEST | 用目录扫描代替，文件多了启动慢 | 版本文件 |
| 5 | 单写者 | 写吞吐受限于一把互斥锁 | 写组（WriteBatch） |

**第 1 条是最重要的**：没有 compaction，写放大低但读放大和空间放大都会一直涨。

## 项目结构

```
├── include/          Slice / Status
├── src/
│   ├── util/         varint 编解码、CRC32C（查表 + 逐位差分测试）
│   ├── wal.*         WAL 读写（四条防线）
│   ├── skiplist.*    跳表
│   ├── memtable.*    MemTable（跳表 + 类型编码）
│   ├── block.*       Block（前缀压缩 + restart）
│   ├── bloom.*       布隆过滤器
│   ├── table.*       SSTable（TableBuilder + Table + footer）
│   └── db.*          DB（Put/Get/Delete + 恢复）
├── tools/
│   ├── db_demo.cpp   命令行工具
│   └── benchmark.cpp 压测
├── tests/            7 组单元测试 + kill -9 驱动
└── docs/             4 份设计文档
```

## 实现过程中的两个 bug

**① 恢复流程的数据丢失窗口**

旧实现：重放 WAL → 立刻开新 WAL（空）→ 保留旧 WAL。
此时再崩溃，重启只会重放那个空的新 WAL，旧 WAL 的数据永久丢失。

修法：重放后**先落盘**再删旧 WAL，保证「数据要么在 SSTable，要么在唯一的 WAL」。
写了回归测试（用旧代码跑确实丢 100 条）。

**② Get 静默跳过坏 SSTable**

旧实现：`Table::Open` / `Table::Get` 失败都 `continue`。
坏文件被悄悄跳过，可能返回更老文件里的**过期旧值**——比报错危险。

修法：只有 `NotFound` 才继续，`Corruption` / `IOError` 一律上报。

## 参考

- [LevelDB 源码](https://github.com/google/leveldb) —— 格式和设计的主要依据
- 《Designing Data-Intensive Applications》第 3 章 —— LSM-Tree 的理论背景
