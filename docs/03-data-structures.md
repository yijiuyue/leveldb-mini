# 数据结构与字节布局（速查）

> **这份文档回答「每个数据长什么样」，`00-architecture.md` 回答「数据怎么流动」。**
> 面试前翻这份就够——每个结构都有：字节图 + 具体例子 + 为什么这么设计。

## 总览：谁嵌套谁

```
SSTable 文件
├── data block 0 ────┐
├── data block 1     │  每个 block = [entry...][restart_offset[]][count]
├── ...              │  每个 entry  = [shared][unshared][vlen][key_delta][value]
├── index block ─────┘  （也是 block，但 entry 的 value 是 (offset,size)）
└── footer 40 字节      [filter_off][filter_size][index_off][index_size][magic]

WAL 文件
└── block（32KB）× N
    └── record = [crc32(4)][len(4)][payload]
        └── payload = [type(1)][klen][key][vlen][value]

内存结构
├── SkipList（MemTable 底层）
│   └── Node = { key, value, height, next[height] }
│       └── value = [type(1)][payload]     ← 和 SSTable 里一样
└── BlockBuilder（构建中）
    └── buffer_（string）+ restarts_（vector<uint32>）
```

---

# 一、WAL 记录

## 1.1 顶层结构

```
┌──────────────┬──────────────┬─────────────────────────┐
│  crc32 (4)   │  length (4)  │  payload (length 字节)   │
└──────────────┴──────────────┴─────────────────────────┘
       ↑                ↑
   小端 uint32      小端 uint32

【crc 覆盖范围】length + payload（不覆盖 crc 自己）
```

**为什么 crc 不覆盖自己**：鸡生蛋——它是算出来的结果。

**为什么 length 放在 crc 之后**：读的时候先校验 crc 再信 length，顺序上更自然（其实都可以）。

## 1.2 payload 的结构

```
┌─────────┬──────────────┬─────────┬──────────────┬────────┐
│ type(1) │ key_len(var) │  key    │ value_len(var)│ value  │
└─────────┴──────────────┴─────────┴──────────────┴────────┘

type: 0x01 = Put, 0x02 = Delete
```

## 1.3 具体例子：`Put("ab", "xy")`

**① DB 层编码 payload**（实测 7 字节）：

```
01           type = kRecPut        1 字节
02           key_len = 2（varint） 1 字节
61 62        "ab"                  2 字节
02           value_len = 2（varint）1 字节
78 79        "xy"                  2 字节
─────────────────────────────────
共 7 字节
```

**② WAL 层加头**（实测 15 字节）：

```
<crc32 4字节>  对上面 7 字节算 CRC32C
07 00 00 00    length = 7（小端）
01 02 61 62 02 78 79   payload
──────────────────────
共 15 字节
```

**对照 `Delete("ab")`**

```
02 02 61 62 00     ← 5 字节（没有 value）
↑  ↑  ↑     ↑
type klen key vlen=0
```

## 1.4 WAL block 的布局（32KB）

```
一个 block = 32768 字节
┌────────────────────────────────────────┐
│ record 0  (15 字节)                     │
│ record 1  (28 字节)                     │
│ ...                                     │
│ 【放不下下一条时】填 0 到 block 末尾      │
└────────────────────────────────────────┘

规则：
  ① 一条记录不能跨 block
  ② 剩余空间 < 8（头部大小）→ 填 0，换 block
  ③ 剩余空间 >= 8 但放不下整条 → 也填 0，换 block
  ④ 读时遇到 crc==0 && len==0 → 识别为填充 → 跳下一个 block
```

**为什么记录不跨 block**：读到坏数据只需跳一个 block，不用维护碎片状态机。代价是平均浪费 2KB/block（约 6%）。

---

# 二、MemTable 的跳表节点

## 2.1 Node 的内存布局

```cpp
struct Node {
    std::string key;                  // 32 字节（libstdc++）
    std::string value;                // 32 字节（libstdc++）
    int height;                       // 4 字节（+4 padding）
    std::atomic<Node*> next[1];       // 8 字节 × height（柔性数组）
};
```

**实际内存（g++ 13.3 / libstdc++ 实测）**：

```
偏移 0           32              64      68      72
┌────────────────┬───────────────┬───────┬────────┬─────────────────┐
│  key (32)      │ value (32)    │height │(pad 4) │ next[0..h-1]    │
└────────────────┴───────────────┴───────┴────────┴─────────────────┘

sizeof(Node) = 80（含 next[1] 的 8 字节）
实际分配     = 72 + 8h = sizeof(Node) + 8*(h-1)

h=1 -> 80    h=2 -> 88    h=3 -> 96
```

**⚠️ `sizeof(std::string)` 依赖标准库实现**：

```
libstdc++（Linux g++）  = 32 字节
libc++（Clang/macOS）   = 24 字节
MSVC（Windows）         = 32 字节
```

**为什么 libstdc++ 是 32**：

```
char*  _M_p          8
size_t _M_length     8
union {
  char   _M_local_buf[16]          ← SSO 缓冲
  size_t _M_allocated_capacity
}                   16
─────────────────────
合计                32
```

**SSO（Small String Optimization）**：≤15 字符的字符串直接存在 `_M_local_buf` 里，**不堆分配**。
所以 `"ab"`、`"xy"` 这种短 key/value **没有额外内存开销**。

**为什么用柔性数组**：`std::vector<Node*>` 要多 24 字节 + 一次额外堆分配。500 万节点差 120MB。

## 2.2 value 的编码

```
┌─────────┬───────────────┐
│ type(1) │  payload      │
└─────────┴───────────────┘
type: 0x01 = 有值（后面跟 value）
      0x00 = 墓碑（后面没东西）
```

**例子**：

```
Put("ab", "xy")      →  Node{ key="ab", value=[01]78 79 }      3 字节
Delete("ab")         →  Node{ key="ab", value=[00] }           1 字节
```

**为什么类型放 value 不放 key**：

```
放 value → 同一个 key 只有一条记录，后写覆盖先写（L1.5 的语义）
放 key   → LevelDB 的「内部键」，同一个 key 可以有多个版本（支持快照）

我选前者，因为 L1.5 不需要快照。
```

---

# 三、Block 的 entry

## 3.1 entry 结构（前缀压缩）

```
┌─────────────┬───────────────┬──────────────┬───────────────┬────────┐
│ shared(var) │ unshared(var) │ value_len(var)│ key_delta     │ value  │
└─────────────┴───────────────┴──────────────┴───────────────┴────────┘

shared    = 和前一条 entry 的公共前缀长度
unshared  = 本条 key 独有的字节数
key_delta = key 的独有部分（不存共享部分）
```

## 3.2 具体例子

**三条 key：`"ab"`、`"abc"`、`"abd"`**

```
entry 0: key="ab"
  shared=0, unshared=2, vlen=3, delta="ab", value=[01]"1"
  → 00 02 03 61 62 01 31            （7 字节）

entry 1: key="abc"（和上一条共享 "ab"）
  shared=2, unshared=1, vlen=3, delta="c", value=[01]"2"
  → 02 01 03 63 01 32               （6 字节）

entry 2: key="abd"（和上一条共享 "ab"）
  shared=2, unshared=1, vlen=3, delta="d", value=[01]"3"
  → 02 01 03 64 01 33               （6 字节）
```

**压缩效果**：三条 key 原本 2+3+3 = 8 字节，压缩后 delta 只占 2+1+1 = 4 字节。

## 3.3 整个 block 的布局

```
┌──────────────────────────┐
│  entry 0                 │
│  entry 1                 │
│  ...                     │
│  entry N-1               │
├──────────────────────────┤
│  restart_offset[0] (4)   │  ← 每 16 条一个，存「字节偏移」
│  restart_offset[1] (4)   │
│  ...                     │
├──────────────────────────┤
│  restart_count (4)       │  ← 最后 4 字节
└──────────────────────────┘

restart_offset_ = size - 4 - count*4      （数组起点 = 数据区终点）
```

## 3.4 restart 点在哪里

```
entry 0    ← restart（shared=0，存完整 key）
entry 1
...
entry 15
entry 16   ← restart
entry 17
...
entry 31
entry 32   ← restart

规律：第 1、17、33、49... 条
公式：restart 个数 = 1 + floor((entry数 - 1) / 16)
```

**为什么需要**：压缩后「第 N 条的 key」必须靠上一条还原 → 顺推 O(n)。restart 让顺推最多 16 条。

---

# 四、Index block

## 4.1 和 data block 的关系

**index block 就是普通的 block**，用同一个 `BlockBuilder` 构建。

**唯一区别：entry 的含义不同。**

| | data block 的 entry | index block 的 entry |
|---|---|---|
| **key** | 用户的 key | **该 data block 的最后一个 key** |
| **value** | `[type][payload]` | **`[block_offset(varint)][block_size(varint)]`** |

## 4.2 具体例子

**假设有两个 data block**：

```
data block 0:  key="ab" .. key="az"    → 偏移 0,   长度 3000
data block 1:  key="ba" .. key="bz"    → 偏移 3000, 长度 2800
```

**index block 的 entry**：

```
entry 0: key="az", value=[0x00][0xB8 0x17]
         ↑ block 0 的最后 key    ↑ offset=0   ↑ size=3000（varint 2 字节）

entry 1: key="bz", value=[0xB8 0x17][0xEE 0x15]
         ↑ block 1 的最后 key    ↑ offset=3000  ↑ size=2800
```

## 4.3 为什么 key 是「最后一个」而不是「第一个」

**证明**：

```
block i 的 key 范围是 (last_key[i-1], last_key[i]]

SeekGE(target) 找「最小的 i 满足 last_key[i] >= target」
→ 因为 i 最小，所以 last_key[i-1] < target
→ target ∈ (last_key[i-1], last_key[i]]
→ 正好是 block i ✅
```

**如果存「第一个 key」**：要找「最后一个 first_key <= target」，是 `upper_bound` 语义，不如 `lower_bound` 自然。

## 4.4 索引的大小

```
每个 data block（4KB）= 约 80 条 entry
每个 block 只贡献 1 条索引（约 30 字节：key 10 + offset 2 + size 2 + 头部 3）

索引占比 = 30 / 4096 ≈ 0.73%
实测：20000 条 / 309KB 数据 / 76 个 block → 索引 0.74%
```

**关键**：0.74% 意味着 309KB 的文件只有 2.3KB 索引 → **整个索引能放内存**。

---

# 五、Footer（40 字节）

## 5.1 字节图

```
偏移 0    ┌──────────────────────┐
          │ filter_offset (8)    │  布隆块起点（L1.5 暂为 0）
偏移 8    ├──────────────────────┤
          │ filter_size   (8)    │
偏移 16   ├──────────────────────┤
          │ index_offset  (8)    │  索引块起点
偏移 24   ├──────────────────────┤
          │ index_size    (8)    │  索引块大小
偏移 32   ├──────────────────────┤
          │ magic         (8)    │  "leveldb:" 的 ASCII
偏移 40   └──────────────────────┘
```

**全是小端 uint64（8 字节定长，不用 varint）**

## 5.2 为什么放文件最后

```
写入时【不知道 index 会多大】（写完才知道 index_size）
→ 放前面就得预留空间或回改 → 破坏「纯追加」

放最后：
  写完 index 后，才知道 offset 和 size
  → 直接追加 footer
  → 整个文件是「纯追加」写的
```

**打开时**：`pread(file_size - 40, 40)` 一次读到 footer，**不用扫全文件**。

## 5.3 magic 的作用

```cpp
const char kMagic[8] = {'l', 'e', 'v', 'e', 'l', 'd', 'b', ':'};
```

**校验「这个文件确实是我的 SSTable」**：

```
用户传了个随机文件路径 → 读 footer → magic 对不上 → 报 Corruption
而不是把垃圾数据当索引解析
```

---

# 六、SSTable 的完整布局

```
偏移 0
┌──────────────────────────────────────┐
│  data block 0                        │
├──────────────────────────────────────┤
│  data block 1                        │
├──────────────────────────────────────┤
│  ...                                 │
├──────────────────────────────────────┤
│  data block N-1                      │
├──────────────────────────────────────┤  ← index_offset
│  index block                         │
├──────────────────────────────────────┤  ← file_size - 40
│  footer (40 字节)                    │
└──────────────────────────────────────┘
file_size
```

## 读一个 key 的完整步骤（带偏移计算）

```
① pread(file_size - 40, 40) → footer
② 解析出 index_offset、index_size
③ pread(index_offset, index_size) → index 字节 → 构造 Block
④ index.SeekGE(target) → (block_offset, block_size)
⑤ pread(block_offset, block_size) → data block 字节 → 构造 Block
⑥ data_block.Get(target) → value
```

**总共 3 次 pread**（footer、index、data block）。

**但 index 在 `Table::Open` 时就加载了 → 实际每次 Get 只要 1 次 pread（data block）。**

---

# 七、varint 编码速查

**本项目大量使用 varint，这里集中说明。**

```
值范围        编码长度    例子
0 .. 127      1 字节      127 → [0x7F]
128 .. 16383  2 字节      300 → [0xAC, 0x02]
16384 ..      3 字节      16384 → [0x80, 0x80, 0x01]

编码规则：
  每字节低 7 位存数据
  最高位 = 1 表示「后面还有」
  最高位 = 0 表示「这是最后一个」

300 = 0b1_00101100
  低 7 位 0101100 = 44，置高位 → 0xAC
  剩余 0b10 = 2，高位 0 → 0x02
```

**哪些字段用 varint**：

```
✓ WAL payload 里的 key_len / value_len
✓ Block entry 里的 shared / unshared / value_len
✓ Index entry 里的 block_offset / block_size

✗ WAL 的 crc32 / length（定长 4 字节，要能直接定位）
✗ Block 的 restart_offset / restart_count（定长 4 字节，要能直接索引）
✗ Footer 的所有字段（定长 8 字节）
```

**规律**：**需要「随机访问」的用定长，只是「顺序读」的用 varint。**

---

# 八、一张总表（所有字段的字节数）

| 结构 | 字段 | 字节数 | 编码 |
|---|---|---|---|
| **WAL 记录** | crc32 | 4 | 定长小端 |
| | length | 4 | 定长小端 |
| | type | 1 | 枚举 |
| | key_len | 1–5 | varint |
| | value_len | 1–5 | varint |
| **WAL block** | 大小 | 32768 | 固定 |
| | 填充 | 0 | 0x00 |
| **跳表 Node** | key | 32 | std::string（libstdc++） |
| | value | 32 | std::string（libstdc++） |
| | height | 4 | int（+4 padding） |
| | next[] | 8×h | atomic 指针（柔性数组） |
| **MemTable value** | type | 1 | 枚举 |
| **Block entry** | shared | 1–5 | varint |
| | unshared | 1–5 | varint |
| | value_len | 1–5 | varint |
| **Block 尾部** | restart_offset[] | 4×R | 定长小端 |
| | restart_count | 4 | 定长小端 |
| **Index entry** | key | — | 同 data block |
| | block_offset | 1–5 | varint |
| | block_size | 1–5 | varint |
| **Footer** | 5 个字段 | 8×5 = 40 | 定长小端 |
| **SSTable** | block_size | 4096 | 配置 |
| | restart_interval | 16 | 配置 |

---

# 九、三个「字段顺序」的设计理由

**面试可能问「为什么这个字段在前面」**：

### ① WAL：crc 在最前

```
读的时候先校验 crc，通过后才相信 length
→ 如果 length 在 crc 之前，可能先按错误长度读取，再发现 crc 错
```

### ② Block entry：shared/unshared 在最前

```
必须先知道「共享多少、独有多少」，才知道 key_delta 占几个字节
→ 读的时候才能正确切分
```

### ③ Footer：magic 在最后

```
如果 magic 在最前，可能和 filter_offset 的前几字节"撞车"
（比如一个真实的 leveldb 文件，filter_offset 恰好 = "leveldb:" 的前 8 字节）
→ 放最后更安全
```

---

# 十、自查清单（面试前问自己）

```
□ WAL 一条记录的完整字节？crc 覆盖哪些字节？
□ WAL block 多大？放不下怎么办？
□ 跳表 Node 有几个字段？next[] 为什么用柔性数组？
□ MemTable 的 value 为什么带类型字节？
□ Block entry 的五个字段？shared 是什么？
□ restart 数组在哪？怎么定位？restart 个数怎么算？
□ Index 的 key 存什么？value 存什么？为什么是「最后一个 key」？
□ Footer 的 40 字节怎么分？为什么放最后？magic 是什么？
□ 一次 Get 读几次盘？为什么不是整个文件？
□ 哪些字段用 varint，哪些用定长？为什么？
```
