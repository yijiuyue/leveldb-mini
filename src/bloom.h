#ifndef LDB_BLOOM_H
#define LDB_BLOOM_H

#include <cstdint>
#include <string>
#include <vector>

#include "slice.h"

// ============ 布隆过滤器 ============
//
// 【解决什么问题】
//   Get 一个不存在的 key，现在要打开每个 SSTable、读 index、读 data block
//   → 每个不存在的 key 都要 1 次 pread（几微秒 + 可能触发磁盘 IO）
//
//   布隆过滤器能在【不读盘】的情况下回答：
//     「这个 key 一定不在」→ 直接跳过，省掉 pread
//     「这个 key 可能存在」→ 老实去查
//
// 【代价】
//   每个 key 占 10 bit = 1.25 字节
//   误判率（假阳性）约 1%：把「不在」误判成「可能存在」
//   → 1% 的查询仍要白读一次 block
//
// 【关键性质】
//   假阳性（false positive）：会。1% 的「不在」被误判成「可能存在」
//   假阴性（false negative）：【绝不会】。存在的 key 一定返回「可能存在」
//   → 所以「一定不在」这个回答是【可信】的，可以安全跳过
//
// 【为什么用双哈希】
//   要 k 个独立哈希函数。用两个基哈希 h1、h2，构造 h1 + i*h2
//   → 一次哈希计算得到全部 k 个位置，省 CPU
//   Kirsch-Mitzenmacher 证明这种构造的误判率与 k 个独立哈希几乎相同

namespace ldb {

// 32 位哈希（LevelDB 同款，MurmurHash 变体）
uint32_t BloomHash(const Slice& key);

class BloomPolicy {
public:
    // bits_per_key = 10 时，k = 0.69 * 10 ≈ 7，误判率约 1%
    explicit BloomPolicy(int bits_per_key = 10);

    // 从「key 的哈希值列表」生成过滤器
    // 格式：[位图 bytes][k（1 字节）]
    std::string CreateFilter(const std::vector<uint32_t>& key_hashes) const;

    // 查询：false = 一定不在；true = 可能存在
    bool MayContain(const Slice& filter, const Slice& key) const;

    int k() const { return k_; }
    int bits_per_key() const { return bits_per_key_; }

private:
    int bits_per_key_;
    int k_;
};

}  // namespace ldb

#endif  // LDB_BLOOM_H