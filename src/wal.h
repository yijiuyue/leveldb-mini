#ifndef LDB_WAL_H
#define LDB_WAL_H

#include <cstdint>
#include <string>

#include "slice.h"
#include "status.h"

// ============ WAL（Write-Ahead Log）============
//
// 作用：在改内存之前先把操作写到磁盘，崩溃后可以重放恢复。
//
// 【为什么必须先写 WAL 再写 MemTable】
//   反过来的话：内存写完，还没写日志就崩了 → 数据永久丢失。
//   正确顺序：先写 WAL（落盘）→ 再改内存 → 崩溃后重放 WAL 重建内存。
//
// 【和 MySQL redo log 的区别】
//   MySQL：数据本来就在磁盘上，redo 是加速 + 保证持久的手段
//   LevelDB：数据在内存 MemTable 里，WAL 是【唯一的磁盘副本】
//   → 所以 MemTable 落成 SSTable 之前，WAL 绝对不能删
//
// ============ 记录格式 ============
//
//   +--------+--------+--------+
//   | crc32  | length |  data  |
//   | 4 字节 | 4 字节 | length |
//   +--------+--------+--------+
//
//   crc 覆盖 [length + data]，不覆盖 crc 自己
//
// ============ 两条规则 ============
//
//   ① 一条记录不能跨 block（block = 32KB）
//      放不下就在 block 末尾填 0，下一条从新 block 开始
//      → 好处：读到坏数据时只需要跳到下一个 block，不用处理碎片状态机
//
//   ② 遇到半条记录（CRC 对不上 / 长度越界）→ 【停在这里】
//      后面的数据都不可信（因为写入是顺序的，前面坏了后面不知道哪对哪）
//
//   ③ block 末尾的填充区：crc=0 且 length=0 → 识别为填充，跳到下一个 block

namespace wal {

constexpr size_t kBlockSize = 32 * 1024;
constexpr size_t kHeaderSize = 8;  // crc(4) + length(4)

// ---------------- 写 ----------------

class Writer {
public:
    Writer() = default;
    ~Writer();
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;

    // 追加模式打开（不存在就创建）
    Status Open(const std::string& path);

    // 追加一条记录（只是 memcpy 到 block 缓冲，不一定落盘）
    Status AddRecord(const Slice& data);

    // 把缓冲刷出去 + fsync（真正落盘）
    Status Sync();

    // 刷缓冲 + 关闭（析构也会调用）
    Status Close();

    uint64_t bytes_written() const { return bytes_written_; }

private:
    Status FlushBlock();  // 把 block_ 写进文件

    std::string path_;
    int fd_ = -1;
    std::string block_;  // 当前 block 的缓冲（< 32KB）
    uint64_t bytes_written_ = 0;
    bool dirty_ = false;  // 有没有未 fsync 的数据
};

// ---------------- 读 ----------------

class Reader {
public:
    Reader() = default;
    ~Reader();
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    Status Open(const std::string& path);

    // 读一条记录
    //   返回 OK          -> *record 有效
    //   返回 IsNotFound  -> 到文件末尾（正常结束）
    //   返回 IsCorruption-> 数据损坏（调用方应该停在这里）
    Status ReadRecord(std::string* record);

    Status Close();

private:
    // 加载下一个 block；没有更多数据返回 false
    bool LoadNextBlock();

    std::string path_;
    int fd_ = -1;
    std::string block_;      // 当前 block 的内容
    size_t block_pos_ = 0;   // 已消费到 block 内的哪个位置
};

}  // namespace wal

#endif  // LDB_WAL_H