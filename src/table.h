#ifndef LDB_TABLE_H
#define LDB_TABLE_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "bloom.h"
#include "block.h"
#include "slice.h"
#include "status.h"

// ============ SSTable（Table）============
//
// 一个【不可变】的有序文件：写完之后只能整体删除，不能修改。
//
// ============ 文件布局 ============
//
//   ┌──────────────────┐  偏移 0
//   │  data block 0    │
//   ├──────────────────┤
//   │  data block 1    │
//   ├──────────────────┤
//   │  ...             │
//   ├──────────────────┤
//   │  data block N-1  │
//   ├──────────────────┤
//   │  index block     │  ← 每个 data block 一条索引
//   ├──────────────────┤
//   │  footer (40字节) │  ← 固定长度，记录 index 的位置
//   └──────────────────┘  文件尾
//
// ============ Footer（固定 40 字节）============
//
//   filter_offset (8)    布隆块起点（暂填 0，布隆后面做）
//   filter_size   (8)
//   index_offset  (8)    索引块起点
//   index_size    (8)    索引块大小
//   magic         (8)    "leveldb:" —— 用来校验「这是不是我的文件」
//
// ============ 为什么 footer 放最后 ============
//   打开文件时 seek 到 size-40 读 40 字节，一次就能定位 index
//   不用扫全文件
//
// ============ 稀疏索引 ============
//   index block 的每条 entry：
//     key   = 该 data block 的【最后一个 key】
//     value = (block_offset, block_size)
//
//   查找时：找「第一个 key >= target」的索引项 → 它的 block 一定含 target
//   为什么？因为该 block 的最后一个 key >= target，
//          而它前一个 block 的最后一个 key < target
//
//   【索引只占数据量的约 0.5%】——所以可以整个加载到内存

namespace ldb {

// ---------------- 写 ----------------

class TableBuilder {
public:
    static constexpr size_t kDefaultBlockSize = 4096;

    explicit TableBuilder(const std::string& path, size_t block_size = kDefaultBlockSize);
    ~TableBuilder();

    TableBuilder(const TableBuilder&) = delete;
    TableBuilder& operator=(const TableBuilder&) = delete;

    // 追加键值对（key 必须严格递增）
    Status Add(const Slice& key, const Slice& value);

    // 收尾：写最后一个 block + index block + footer + fsync
    Status Finish();

    size_t NumEntries() const { return num_entries_; }
    size_t NumDataBlocks() const { return num_data_blocks_; }
    uint64_t FileSize() const { return offset_; }

private:
    Status Flush();  // 把当前 data block 写出去，并在 index 里加一条
    Status WriteRaw(const char* data, size_t n);

    std::string path_;
    int fd_ = -1;
    size_t block_size_;
    uint64_t offset_ = 0;  // 已写字节数（= 下一个 block 的起始偏移）

    BlockBuilder data_block_;
    BlockBuilder index_block_;

    std::string last_key_;
    bool has_last_key_ = false;
    std::string pending_index_key_;  // 当前 block 的最后一个 key

    // 布隆过滤器：记录每个 key 的哈希，Finish 时一次性生成位图
    std::vector<uint32_t> key_hashes_;
    BloomPolicy bloom_policy_;

    size_t num_entries_ = 0;
    size_t num_data_blocks_ = 0;
    bool finished_ = false;
    Status status_;
};

// ---------------- 读 ----------------

class Table {
public:
    Table();
    ~Table();

    Table(const Table&) = delete;
    Table& operator=(const Table&) = delete;

    static Status Open(const std::string& path, std::unique_ptr<Table>* table);

    // 查找
    //   Status::OK         -> 找到，*value 有效
    //   Status::NotFound   -> 不存在
    Status Get(const Slice& key, std::string* value) const;

private:
    // 读文件的 [offset, offset+size) 到 *out
    Status ReadRange(uint64_t offset, uint64_t size, std::string* out) const;
    // 读一个 data block 的内容
    Status ReadBlock(uint64_t offset, uint64_t size, std::string* out) const;

    std::string path_;
    int fd_ = -1;
    uint64_t file_size_ = 0;

    // 【成员顺序重要】index_data_ 必须声明在 index_ 之前
    // 因为 Block 的 Slice 指向 index_data_ 的内存，析构顺序反了就悬垂
    std::string index_data_;
    std::unique_ptr<Block> index_;

    // 布隆过滤器（整文件一个）
    // filter_data_ 同样必须声明在 bloom_policy_ 之前（Slice 指向它）
    std::string filter_data_;
    BloomPolicy bloom_policy_;
};

}  // namespace ldb

#endif  // LDB_TABLE_H