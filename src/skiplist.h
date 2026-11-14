#ifndef LDB_SKIPLIST_H
#define LDB_SKIPLIST_H

#include <atomic>
#include <cstdint>
#include <string>

// ============ 跳表（SkipList）============
//
// 【为什么 MemTable 用跳表，不用红黑树】
//   ① 有序：能顺序遍历 -> 落盘时天然有序（SSTable 要求有序）
//   ② 插入/查找都是 O(log n)，和红黑树一样
//   ③ 实现简单：不用旋转/变色，代码量少一半
//   ④ 并发友好：插入只改【局部几层的指针】，红黑树旋转会影响一大片
//
// 【并发模型】（和 LevelDB 一致）
//   写：单线程（由 DB 层的互斥锁保证「同一时刻只有一个写者」）
//   读：无锁（memory_order 保证可见性）
//     - 写指针用 memory_order_release（发布）
//     - 读指针用 memory_order_acquire（获取）
//   这就是 C++ 八股里「release 发布 / acquire 获取」的真实用法。
//
// 【数据所有权】
//   节点自己持有 std::string（key 和 value 各一次堆分配）。
//   LevelDB 用 Arena 把这些分配集中管理，我这里为了简单用 std::string。
//
// 【为什么用「柔性数组」存各层指针】
//   每个节点高度不同，用 std::vector 会多 24 字节开销 + 一次额外分配。
//   next[1] + 额外分配空间 = 各层指针和节点在一块内存里。

namespace ldb {

class SkipList {
private:
    // 【注意】Node 必须在 Iterator 之前定义
    //         因为 Iterator 的方法内联访问 node_->key，需要完整类型
    struct Node {
        std::string key;
        std::string value;
        int height;
        std::atomic<Node*> next[1];  // 实际长度 = height

        Node(const std::string& k, const std::string& v, int h)
            : key(k), value(v), height(h) {}
    };

public:
    // 最大 12 层，每层上升概率 1/4 -> 可覆盖 4^12 ≈ 1600 万个元素
    static constexpr int kMaxHeight = 12;

    explicit SkipList(int (*cmp)(const std::string&, const std::string&));
    ~SkipList();

    SkipList(const SkipList&) = delete;
    SkipList& operator=(const SkipList&) = delete;

    // 插入（若 key 已存在，value 会被更新）
    // 【调用方必须保证同一时刻只有一个写者】
    void Insert(const std::string& key, const std::string& value);

    // 查找：找到返回 true
    bool Get(const std::string& key, std::string* value) const;

    int size() const { return count_; }
    size_t ApproximateMemoryUsage() const { return memory_usage_; }

    // ---------------- 迭代器（落盘时顺序遍历） ----------------
    class Iterator {
    public:
        explicit Iterator(const SkipList* list) : list_(list), node_(nullptr) {}
        bool Valid() const { return node_ != nullptr; }
        const std::string& key() const { return node_->key; }
        const std::string& value() const { return node_->value; }
        void Next() { node_ = node_->next[0].load(std::memory_order_acquire); }
        void SeekToFirst() {
            node_ = list_->head_->next[0].load(std::memory_order_acquire);
        }

    private:
        const SkipList* list_;
        Node* node_;
    };

private:
    Node* NewNode(const std::string& key, const std::string& value, int height);
    int RandomHeight();
    // 找到「最后一个 key < target」的节点；同时填充 prev[level]
    Node* FindLessThan(const std::string& target, Node** prev) const;

    int (*cmp_)(const std::string&, const std::string&);

    Node* head_;
    int max_height_;
    int count_ = 0;
    size_t memory_usage_ = 0;
    uint32_t rng_state_ = 0x12345678u;  // xorshift32 的状态

    friend class Iterator;
};

}  // namespace ldb

#endif  // LDB_SKIPLIST_H