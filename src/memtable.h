#ifndef LDB_MEMTABLE_H
#define LDB_MEMTABLE_H

#include <string>

#include "skiplist.h"

// ============ MemTable ============
//
// 内存里的有序键值表，底层是跳表。
//
// 【为什么需要它】
//   写操作不能直接落成 SSTable（要攒够 + 排序，慢）。
//   先写这里（内存 + 有序），攒满 1MB 再整体落盘。
//
// 【删除怎么表示】
//   不真删，而是写一条「墓碑」（type = kTypeDeletion）。
//   读取时看到墓碑 -> 返回「明确不存在」，并且【不再往更老的文件找】。
//
// 【内部编码】
//   跳表的 value 第 1 个字节是类型：
//     0x01 = 有值（后面跟 value）
//     0x00 = 墓碑（后面没东西）
//   这样 put 和 delete 共用一个跳表。

namespace ldb {

enum ValueType : unsigned char {
    kTypeDeletion = 0x0,
    kTypeValue = 0x1,
};

// 查询结果
struct MemTableResult {
    enum Kind { kNotFound, kFound, kDeleted } kind = kNotFound;
    std::string value;
};

class MemTable {
public:
    MemTable();
    ~MemTable();

    MemTable(const MemTable&) = delete;
    MemTable& operator=(const MemTable&) = delete;

    void Put(const std::string& key, const std::string& value);
    void Delete(const std::string& key);

    // 查找
    //   kFound    -> result->value 有效
    //   kDeleted  -> 找到墓碑（明确不存在）
    //   kNotFound -> 这个 MemTable 里没有
    MemTableResult::Kind Get(const std::string& key, std::string* value) const;

    size_t ApproximateMemoryUsage() const { return table_.ApproximateMemoryUsage(); }
    int size() const { return table_.size(); }

    // 落盘时用：顺序遍历所有键值对
    // 回调签名：fn(key, type, payload)
    template <typename Fn>
    void ForEach(Fn fn) const {
        SkipList::Iterator it(&table_);
        for (it.SeekToFirst(); it.Valid(); it.Next()) {
            const std::string& encoded = it.value();
            const unsigned char type = static_cast<unsigned char>(encoded[0]);
            const std::string payload = encoded.substr(1);
            fn(it.key(), static_cast<ValueType>(type), payload);
        }
    }

private:
    SkipList table_;
};

}  // namespace ldb

#endif  // LDB_MEMTABLE_H