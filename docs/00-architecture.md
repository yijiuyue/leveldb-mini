# 整体架构：一条数据的一生

> **面试前看这份 10 分钟，整条链路就清晰了。**
> 不要背「有 6 个模块」——背**这条问题链**。

## 一、因果链：每个模块都是为了解决前一个模块的问题

```
【需求】数据要存下来，重启不丢
   ↓ 方案：写文件
【问题 1】直接改文件是「随机写」，慢
   ↓ 方案：先追加日志（顺序写）→ 【WAL】
【问题 2】每次读都要从头扫日志，O(n)
   ↓ 方案：内存里维护一份有序数据 → 【MemTable（跳表）】
【问题 3】内存装不下
   ↓ 方案：内存满了落成「有序文件」→ 【SSTable】
【问题 4】有序文件里有几万条，怎么快速定位？
   ↓ 方案：切成 4KB 的块，块内 key 有序 + 前缀压缩 → 【Block】
【问题 5】文件里有几十个 block，怎么找？
   ↓ 方案：每个 block 建一条索引 → 【index block】
【问题 6】索引放在文件的哪里？读的时候怎么找到它？
   ↓ 方案：把索引位置写在文件最后 40 字节 → 【Footer】
【问题 7】重启后怎么恢复内存里的数据？
   ↓ 方案：重放 WAL → 【Recovery】
```

**每一环都是「上一环的代价」逼出来的。**

## 二、分层图

```
┌─────────────────────────────────────────┐
│  DB 层（db.cpp）                        │  Put / Get / Delete
│    · 单写者锁                            │  Open / Recovery
│    · 决定什么时候落盘                     │
└───────┬─────────────────────┬───────────┘
        │ 写                   │ 读
        ▼                     ▼
┌──────────────┐      ┌──────────────┐
│  WAL（wal）  │      │  MemTable    │  ← 内存，跳表
│  顺序追加     │      │  （skiplist）│
│  CRC 校验     │      └──────┬───────┘
└──────────────┘             │ 满了
                             ▼
                    ┌──────────────────┐
                    │  TableBuilder    │
                    │    ↓             │
                    │  BlockBuilder    │  ← 前缀压缩 + restart
                    │    ↓             │
                    │  data block(4KB) │
                    │    ↓             │
                    │  index block     │  ← 稀疏索引
                    │    ↓             │
                    │  footer(40字节)   │
                    └──────────────────┘
                             │
                             ▼  写到磁盘
                    ┌──────────────────┐
                    │  SSTable 文件     │
                    │  xxx.sst         │
                    └──────────────────┘
```

## 三、一条 Put 的完整路径

```
db->Put("name", "alice", sync=true)
  │
  ├─ ① 【DB】加锁（单写者）
  │
  ├─ ② 【DB】编码成 WAL 记录
  │     01 | 04 | "name" | 05 | "alice"
  │     ↑type ↑klen         ↑vlen
  │
  ├─ ③ 【WAL】AddRecord
  │     → 加头：[crc32(4)][len(4)] + 上面那串
  │     → 追加到 block_ 缓冲（32KB）
  │     → sync=true → 【fsync 落盘】  ★ 到这一步数据才真的安全
  │
  ├─ ④ 【MemTable】Put
  │     → value 编码成 [0x01]"alice"（多 1 字节类型）
  │     → 【跳表】按 key 有序插入
  │
  ├─ ⑤ 【DB】ApproximateMemoryUsage() >= 4MB？
  │     否 → 返回 OK
  │     是 → ⑥
  │
  └─ ⑥ 【DB】FlushMemTable()
        ├─ 【TableBuilder】逐个 Add（MemTable 已有序）
        │     → 【BlockBuilder】算公共前缀 → 写 entry
        │     → 攒到 4KB → Flush 成 data block
        │     → 【index block】加一条（key = 本 block 最后 key）
        ├─ 【TableBuilder】Finish → 写 index → 写 footer → fsync
        ├─ 清空 MemTable
        └─ 删旧 WAL，开新 WAL
```

**★ ③ 必须在 ④ 之前——这是整个设计的命门。**

## 四、一条 Get 的完整路径

```
db->Get("name", &v)
  │
  ├─ ① 【DB】加锁
  │
  ├─ ② 【MemTable】Get
  │     → 【跳表】FindLessThan + 相等判断
  │     → 找到 → 解码 type
  │          0x01 → 返回 value ✅
  │          0x00 → 墓碑 → NotFound（**停止**）
  │     → 没找到 → 继续
  │
  ├─ ③ 遍历 SSTable（**从新到旧**）
  │     对每个文件：
  │       【Table::Open】
  │         → pread 最后 40 字节 = footer
  │         → 解析 index_offset / index_size
  │         → 校验 magic
  │         → pread index block → 【Block】构造
  │       【Table::Get】
  │         → 【index block】SeekGE
  │              → 二分 restart → 顺推 ≤16 条
  │              → 第一个 key >= "name" 的条目
  │              → 得到 (block_offset, block_size)
  │         → pread 那个 data block
  │         → 【Block】Get
  │              → 二分 restart → 顺推 → 解码前缀 → 完整 key
  │              → 相等 → 返回 value（含 type）
  │         → 解 type：0x01 → 返回 / 0x00 → 墓碑 → NotFound（停止）
  │
  └─ ④ 都没有 → NotFound
```

**注意：`【Block】` 被用了两次**——一次是 index block，一次是 data block。**同一个数据结构，两种用途。**

## 五、重启恢复的路径

```
DB::Open("/tmp/mydb")
  │
  ├─ ① 【std::filesystem】扫描目录
  │     → 收集 *.sst（排序，编号越大越新）
  │     → 收集 *.log
  │
  ├─ ② 【WAL Reader】重放编号最大的 .log
  │     while (ReadRecord(&rec))：
  │        【四条防线】
  │          · 剩余 < 8 字节        → 换 block（continue）
  │          · 解析失败             → 换 block（continue）
  │          · crc==0 && len==0     → 填充区 → 换 block（continue）
  │          · 长度越界             → Corruption → **停**
  │          · CRC 对不上           → Corruption → **停**
  │        → 解码 (type, key, value) → 【MemTable】Put / Delete
  │
  ├─ ③ 删掉陈旧的 .log（数据已在 SSTable 里）
  │
  └─ ④ 开新 WAL，返回 DB 对象
```

**「停」= 静默截断**——前面恢复的都可信，后面不知道在哪。

## 六、模块职责表（一句话）

| 模块 | 文件 | 一句话职责 | 解决什么 |
|---|---|---|---|
| **WAL** | `wal.h/.cpp` | 追加日志 + CRC，崩溃可重放 | 随机写慢 |
| **MemTable** | `memtable.h/.cpp` + `skiplist.h/.cpp` | 内存里的有序表 | 读日志 O(n) |
| **Block** | `block.h/.cpp` | 4KB 有序键值块，前缀压缩 | 文件内快速定位 |
| **Table** | `table.h/.cpp` | data blocks + 稀疏索引 + footer | 文件内跨 block 定位 |
| **DB** | `db.h/.cpp` | 串起四层 + 恢复 | 对外 API |

## 七、同一条数据在每一层的形态（这个最能帮你串起来）

**`"name" = "alice"` 经过 5 层，每层加自己的「信封」**：

```
【你的视角】
  Put("name", "alice")

【DB → WAL】
  01 04 6E616D65 05 616C696365
  ↑type ↑klen  "name"  ↑vlen  "alice"
  + 头部 [crc32 4字节][len 4字节]

【DB → MemTable】
  跳表节点：key = "name"
            value = 01 616C696365     ← 多了 1 字节类型

【FlushMemTable → BlockBuilder】
  entry:
    shared_len   = 0        （前面没有同前缀的 key）
    unshared_len = 4
    value_len    = 6        （1 字节类型 + 5 字节 value）
    key_delta    = "name"
    value        = 01 616C696365

【Block → data block】
  [entry 0][entry 1]...[restart 数组][restart_count]

【TableBuilder → SSTable 文件】
  [data block 0][data block 1]...[index block][footer 40字节]
                                         ↑
                              key = "name"（若它是本 block 最后一个）
                              value = (offset, size)
```

**面试官问「你的数据流是怎样的」，讲这个演进过程，比讲「我有 6 个模块」强十倍。**

## 八、怎么快速串起来（三步练习）

**第一步（10 分钟）**：把「一、因果链」那 7 行抄一遍，理解「每一环为什么存在」。

**第二步（15 分钟）**：**手画**「一条 Put 的路径」，只画方框和箭头：
```
Put → WAL → MemTable → (满了) → BlockBuilder → data block → index → footer
```

**第三步（10 分钟）**：**手画**「一条 Get 的路径」：
```
Get → MemTable → (没有) → Table::Open → footer → index → SeekGE → data block → Block::Get
```

**能自己画出这两张图，就串起来了。**

## 九、三个「同一套逻辑出现三次」的地方

**这三个是「理解的关键」，因为它们说明设计的一致性**：

| 模式 | 出现位置 | 具体 |
|---|---|---|
| **元数据放尾部** | Block 的 restart 数组 / Table 的 footer | 都是「先读尾部的定长元数据，再定位前面的数据」 |
| **二分 + 顺推** | Block 内部 / index block | 都是「restart 二分 → 顺推 ≤16 条」 |
| **类型前缀** | MemTable 的 value / SSTable 的 value | 都是 `[type 1字节][payload]`，为了表达墓碑 |

**面试时主动指出这三处「同构」，会让人觉得你理解得很系统。**

## 十、已知限制（必须主动说）

| # | 限制 | 后果 |
|---|---|---|
| 1 | **没有 compaction** | SSTable 只增不减 → 读放大 O(文件数)。实测 2000 条产生 50 个文件 |
| 2 | 每次 Get 都 open/close Table | 多 2 个系统调用（真实实现用 TableCache） |
| 3 | 读也加锁 | 并发读性能差（真实实现读无锁） |
| 4 | 没有布隆过滤器 | 不存在的 key 也要读盘（*做完后删掉这条*） |
| 5 | `Get` 遇到坏 SSTable 就 continue | **静默丢数据**——真实实现应报 Corruption |
