#include "bloom.h"

#include <cstring>

#include "util/coding.h"

namespace ldb {

uint32_t BloomHash(const Slice& key) {
    // LevelDB 的 Hash 函数（MurmurHash 变体的简化版）
    const uint32_t m = 0xc6a4a793;
    const uint32_t r = 24;
    const char* data = key.data();
    size_t n = key.size();

    uint32_t h = 0xbc9f1d34 ^ (static_cast<uint32_t>(n) * m);

    while (n >= 4) {
        uint32_t w = 0;
        std::memcpy(&w, data, 4);   // 小端读 4 字节
        h += w;
        h *= m;
        h ^= (h >> 16);
        data += 4;
        n -= 4;
    }

    // 处理尾部 1-3 字节（故意的 fallthrough）
    switch (n) {
        case 3:
            h += static_cast<uint32_t>(static_cast<unsigned char>(data[2])) << 16;
            [[fallthrough]];
        case 2:
            h += static_cast<uint32_t>(static_cast<unsigned char>(data[1])) << 8;
            [[fallthrough]];
        case 1:
            h += static_cast<uint32_t>(static_cast<unsigned char>(data[0]));
            h *= m;
            h ^= (h >> r);
            break;
        default:
            break;
    }
    return h;
}

BloomPolicy::BloomPolicy(int bits_per_key) : bits_per_key_(bits_per_key) {
    // k = ln2 * bits_per_key ≈ 0.69 * bits_per_key
    k_ = static_cast<int>(bits_per_key * 0.69);
    if (k_ < 1) k_ = 1;
    if (k_ > 30) k_ = 30;
}

std::string BloomPolicy::CreateFilter(const std::vector<uint32_t>& key_hashes) const {
    // 位图大小：n 个 key × bits_per_key，向上取整到字节
    size_t bits = key_hashes.size() * static_cast<size_t>(bits_per_key_);
    if (bits < 64) bits = 64;               // 至少 8 字节
    size_t bytes = (bits + 7) / 8;
    bits = bytes * 8;                        // 实际位数

    std::string filter;
    filter.resize(bytes + 1);
    char* array = &filter[0];
    std::memset(array, 0, bytes);

    for (uint32_t h : key_hashes) {
        // 用 h 和 delta 生成 k 个位置（双哈希）
        // delta 是「h 循环右移 17 位」，保证和 h 差异够大
        uint32_t delta = (h >> 17) | (h << 15);
        for (int i = 0; i < k_; i++) {
            const uint32_t bitpos = h % static_cast<uint32_t>(bits);
            array[bitpos / 8] = static_cast<char>(
                static_cast<unsigned char>(array[bitpos / 8]) | (1u << (bitpos % 8)));
            h += delta;
        }
    }

    // 最后一个字节存 k
    filter[bytes] = static_cast<char>(k_);
    return filter;
}

bool BloomPolicy::MayContain(const Slice& filter, const Slice& key) const {
    const size_t len = filter.size();
    if (len < 2) return false;               // 空过滤器 / 太短 -> 认为没有

    const char* array = filter.data();
    const size_t bits = (len - 1) * 8;
    const int k = static_cast<unsigned char>(array[len - 1]);
    if (k > 30) {
        // k 太大说明过滤器损坏或格式不对 —— 保守返回 true（可能含）
        // 【为什么保守】宁可多读一次盘，也不能漏掉真实存在的 key
        return true;
    }

    uint32_t h = BloomHash(key);
    uint32_t delta = (h >> 17) | (h << 15);
    for (int i = 0; i < k; i++) {
        const uint32_t bitpos = h % static_cast<uint32_t>(bits);
        if ((static_cast<unsigned char>(array[bitpos / 8]) & (1u << (bitpos % 8))) == 0) {
            return false;                    // 有一位是 0 -> 一定不在
        }
        h += delta;
    }
    return true;                             // 全部是 1 -> 可能存在
}

}  // namespace ldb