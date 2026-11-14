#include "skiplist.h"

#include <new>

namespace ldb {

SkipList::SkipList(int (*cmp)(const std::string&, const std::string&)) : cmp_(cmp) {
    head_ = NewNode(std::string(), std::string(), kMaxHeight);
    max_height_ = 1;
}

SkipList::~SkipList() {
    Node* cur = head_;
    while (cur != nullptr) {
        Node* next = cur->next[0].load(std::memory_order_relaxed);
        cur->~Node();
        delete[] reinterpret_cast<char*>(cur);
        cur = next;
    }
}

SkipList::Node* SkipList::NewNode(const std::string& key, const std::string& value,
                                  int height) {
    // 一次分配：节点本体 + (height-1) 个额外指针
    const size_t size = sizeof(Node) + sizeof(std::atomic<Node*>) * (height - 1);
    char* mem = new char[size];
    memory_usage_ += size + key.size() + value.size();

    Node* node = new (mem) Node(key, value, height);
    for (int i = 0; i < height; i++) {
        new (&node->next[i]) std::atomic<Node*>(nullptr);
    }
    return node;
}

int SkipList::RandomHeight() {
    // xorshift32：比 rand() 快，且状态自持（不依赖全局种子）
    uint32_t x = rng_state_;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_state_ = x;

    int height = 1;
    // 每层 1/4 概率上升（取 2 位，等于 0 才上升）
    while (height < kMaxHeight && (x & 3) == 0) {
        height++;
        x >>= 2;
    }
    return height;
}

SkipList::Node* SkipList::FindLessThan(const std::string& target, Node** prev) const {
    Node* x = head_;
    int level = max_height_ - 1;
    Node* candidate = nullptr;

    while (true) {
        Node* next = x->next[level].load(std::memory_order_acquire);
        if (next != nullptr && cmp_(next->key, target) < 0) {
            x = next;  // 当前层还能往前走
        } else {
            if (prev != nullptr) prev[level] = x;
            if (level == 0) {
                candidate = next;  // 第 0 层的下一个（可能 == target）
                break;
            }
            level--;
        }
    }
    return candidate;
}

void SkipList::Insert(const std::string& key, const std::string& value) {
    Node* prev[kMaxHeight];
    Node* candidate = FindLessThan(key, prev);

    // key 已存在 -> 更新 value（指针不变，因为节点地址没动）
    if (candidate != nullptr && cmp_(candidate->key, key) == 0) {
        candidate->value = value;
        return;
    }

    const int height = RandomHeight();
    if (height > max_height_) {
        for (int i = max_height_; i < height; i++) {
            prev[i] = head_;  // 新层的 prev 都是头节点
        }
        max_height_ = height;
    }

    Node* node = NewNode(key, value, height);

    // 逐层挂上去
    // release：保证「node 的内容已写好」对读者可见
    for (int i = 0; i < height; i++) {
        node->next[i].store(prev[i]->next[i].load(std::memory_order_relaxed),
                            std::memory_order_relaxed);
        prev[i]->next[i].store(node, std::memory_order_release);
    }
    count_++;
}

bool SkipList::Get(const std::string& key, std::string* value) const {
    Node* candidate = FindLessThan(key, nullptr);
    if (candidate != nullptr && cmp_(candidate->key, key) == 0) {
        *value = candidate->value;
        return true;
    }
    return false;
}

}  // namespace ldb