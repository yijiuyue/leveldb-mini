#ifndef LDB_BLOCK_H
#define LDB_BLOCK_H

#include <cstdint>
#include <string>
#include <vector>

#include "slice.h"

// ============ SSTable 的 Block（数据块）============
//
// 一个 block 里存的是「按键有序的键值对」，默认 4KB。
//
// ============ 格式 ============
//
//   ┌──────────────────────┐
//   │ entry 0              │  ← 前缀压缩
//   │ entry 1              │
//   │ ...                  │
//   │ entry N-1            │
//   ├──────────────────────┤
//   │ restart_offset[0] (4)│  ← 每 16 条一个 restart
//   │ ...                  │
//   │ restart_offset[R-1](4)│
//   ├──────────────────────┤
//   │ restart_count (4)    │
//   └──────────────────────┘
//
//   entry 格式：
//     shared_len   (varint)   和前一条的公共前缀长度
//     unshared_len (varint)   本条独有的长度
//     value_len    (varint)
//     unshared_key (bytes)    独有部分
//     value        (bytes)
//
// ============ 两个关键设计 ============
//
// ① 【前缀压缩】
//    相邻 key 通常共享前缀（user:1001 / user:1002），
//    只存「不同的部分」，省 30-60% 空间。
//
// ② 【restart point】
//    压缩后要读第 N 条必须从第 0 条顺推。
//    每 16 条设一个 restart（shared_len = 0，存完整 key），
//    查找时「二分 restart + 最多顺推 16 条」。
//
// ============ 为什么 restart 存的是「字节偏移」不是「第几条」============
//    直接拿去算指针位置，不用再遍历一遍。
//    代价：每条 restart 占 4 字节（16 条 entry 才一个，可忽略）。

namespace ldb {

constexpr int kDefaultRestartInterval = 16;

// ---------------- 构建 ----------------

class BlockBuilder {
public:
    explicit BlockBuilder(int restart_interval = kDefaultRestartInterval);

    // 追加一条（key 必须【严格递增】）
    void Add(const Slice& key, const Slice& value);

    // 收尾：写入 restart 数组 + count，返回完整 block 内容
    // 【调用后不能继续 Add】
    Slice Finish();

    // 当前估计大小（用于判断「这个 block 满了吗」）
    size_t CurrentSizeEstimate() const;

    void Reset();
    bool empty() const { return buffer_.empty(); }
    int entries() const { return counter_ + finished_entries_; }

private:
    std::string buffer_;             // 所有 entry 的字节
    std::vector<uint32_t> restarts_; // restart 点的字节偏移
    int restart_interval_;
    int counter_ = 0;                // 距离下一个 restart 还差几条
    int finished_entries_ = 0;
    std::string last_key_;           // 上一条的完整 key（用于算公共前缀）
    bool finished_ = false;
};

// ---------------- 读取 ----------------

class Block {
public:
    // 构造时解析尾部的 restart 数组
    explicit Block(Slice contents);

    // 查找 key（精确匹配）
    //   找到 -> 返回 true，*value 填上
    //   没找到 -> false
    bool Get(const Slice& key, std::string* value) const;

    // 查找【第一个 key >= target】的 entry
    //   index block 用这个：它存的是「每个 data block 的最后一个 key」，
    //   找「第一个 >= target 的索引项」就定位到了目标 block
    //   找到 -> true，*out_key / *out_value 填上
    //   没有（target 比所有 key 都大）-> false
    bool SeekGE(const Slice& target, std::string* out_key, std::string* out_value) const;

    // 遍历：从 first_key 开始，最多拿 n 条（用于调试/测试）
    void DumpTo(std::vector<std::pair<std::string, std::string>>* out) const;

    size_t size() const { return contents_.size(); }
    uint32_t NumRestarts() const { return num_restarts_; }
    uint32_t NumEntries() const { return num_entries_; }

private:
    // 取第 i 个 restart 的字节偏移
    uint32_t RestartOffset(uint32_t i) const;

    // 解析一条 entry（shared/non_shared/value_len + 数据）
    // 成功返回 true，并把 *input 前移
    static bool ParseEntry(Slice* input, uint32_t* shared, Slice* key_delta,
                           Slice* value);

    // 比较第 i 个 restart 点的 key 和 target
    //   返回 <0 / 0 / >0
    int CompareRestartKey(uint32_t i, const Slice& target) const;

    Slice contents_;
    uint32_t restart_offset_ = 0;  // restart 数组在 contents_ 里的起点
    uint32_t num_restarts_ = 0;
    uint32_t num_entries_ = 0;
};

}  // namespace ldb

#endif  // LDB_BLOCK_H