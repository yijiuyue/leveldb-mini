// 布隆过滤器测试
//
// 重点：
//   ① 没有假阴性（存在的 key 一定 MayContain = true）  ← 最关键
//   ② 假阳性率接近理论值（约 1%）
//   ③ 过滤器大小符合预期（10 bit/key）
//   ④ SSTable 集成：不存在的 key 不读 data block
//   ⑤ 空过滤器 / 损坏过滤器的行为

#include <cstdio>
#include <string>
#include <vector>

#include "bloom.h"
#include "slice.h"

static int g_failed = 0;
static int g_total = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        g_total++;                                                        \
        if (!(cond)) {                                                    \
            g_failed++;                                                   \
            std::printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); \
        }                                                                 \
    } while (0)

namespace {

std::string Key(int i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key_%08d", i);
    return buf;
}

// ---------------- ① 无假阴性（最重要） ----------------

void TestNoFalseNegative() {
    std::printf("[1] 无假阴性（存在的 key 一定命中）\n");
    ldb::BloomPolicy policy(10);

    std::vector<std::string> keys;
    std::vector<uint32_t> hashes;
    for (int i = 0; i < 10000; i++) {
        keys.push_back(Key(i));
        hashes.push_back(ldb::BloomHash(Slice(keys.back())));
    }

    const std::string filter = policy.CreateFilter(hashes);
    std::printf("       10000 个 key -> 过滤器 %zu 字节（%.2f 字节/key）\n",
                filter.size(), double(filter.size()) / 10000.0);

    int missed = 0;
    for (const auto& k : keys) {
        if (!policy.MayContain(Slice(filter), Slice(k))) missed++;
    }
    CHECK(missed == 0);   // 【绝不】能漏
    std::printf("       10000 个存在的 key，漏判 %d 个（必须是 0）\n", missed);
}

// ---------------- ② 假阳性率 ----------------

void TestFalsePositiveRate() {
    std::printf("[2] 假阳性率（理论约 1%%）\n");
    ldb::BloomPolicy policy(10);

    const int kN = 10000;
    std::vector<uint32_t> hashes;
    for (int i = 0; i < kN; i++) {
        hashes.push_back(ldb::BloomHash(Slice(Key(i))));
    }
    const std::string filter = policy.CreateFilter(hashes);

    // 查 10000 个【不存在】的 key
    int false_positive = 0;
    for (int i = kN; i < kN * 2; i++) {
        if (policy.MayContain(Slice(filter), Slice(Key(i)))) false_positive++;
    }
    const double rate = 100.0 * false_positive / kN;
    std::printf("       查询 10000 个不存在的 key，误判 %d 个（%.2f%%）\n",
                false_positive, rate);

    CHECK(rate < 5.0);    // 10 bit/key 理论约 1%，留余量
}

// ---------------- ③ bits_per_key 的影响 ----------------

void TestBitsPerKey() {
    std::printf("[3] bits_per_key 对误判率的影响\n");
    const int kN = 5000;

    std::vector<uint32_t> hashes;
    for (int i = 0; i < kN; i++) hashes.push_back(ldb::BloomHash(Slice(Key(i))));

    for (int bpk : {4, 6, 10, 14}) {
        ldb::BloomPolicy policy(bpk);
        const std::string filter = policy.CreateFilter(hashes);

        int fp = 0;
        for (int i = kN; i < kN * 2; i++) {
            if (policy.MayContain(Slice(filter), Slice(Key(i)))) fp++;
        }
        std::printf("       %2d bit/key -> k=%d, %5zu 字节, 误判 %.2f%%\n",
                    bpk, policy.k(), filter.size(), 100.0 * fp / kN);
        CHECK(fp < kN / 2);   // 无论哪个档位都不能过半
    }
}

// ---------------- ④ 空 / 极小 / 损坏 ----------------

void TestEdgeCases() {
    std::printf("[4] 边界情况\n");
    ldb::BloomPolicy policy(10);

    // 空 key 列表
    {
        const std::string filter = policy.CreateFilter({});
        CHECK(filter.size() == 9);   // 8 字节位图（最小 64 bit）+ 1 字节 k
        CHECK(!policy.MayContain(Slice(filter), Slice("anything")));
    }

    // 单个 key
    {
        std::vector<uint32_t> one{ldb::BloomHash(Slice("only"))};
        const std::string filter = policy.CreateFilter(one);
        CHECK(policy.MayContain(Slice(filter), Slice("only")));
    }

    // 太短的过滤器（< 2 字节）→ 认为没有
    {
        const std::string tiny = "x";
        CHECK(!policy.MayContain(Slice(tiny), Slice("k")));
    }

    // 损坏的过滤器（k 写成 200）→ 保守返回 true
    {
        std::string bad = policy.CreateFilter({ldb::BloomHash(Slice("a"))});
        bad[bad.size() - 1] = static_cast<char>(200);
        CHECK(policy.MayContain(Slice(bad), Slice("随便什么")));   // 保守
    }
}

// ---------------- ⑤ 哈希函数基本性质 ----------------

void TestHash() {
    std::printf("[5] 哈希函数\n");

    // 相同输入 -> 相同输出
    CHECK(ldb::BloomHash(Slice("hello")) == ldb::BloomHash(Slice("hello")));

    // 不同输入 -> 不同输出（大概率）
    CHECK(ldb::BloomHash(Slice("hello")) != ldb::BloomHash(Slice("hellp")));
    CHECK(ldb::BloomHash(Slice("")) != ldb::BloomHash(Slice("a")));

    // 空字符串也能处理
    (void)ldb::BloomHash(Slice(""));

    // 长字符串
    std::string long_key(1000, 'x');
    (void)ldb::BloomHash(Slice(long_key));

    // 分布大致均匀（10000 个 key 的哈希，看最高位是 0/1 的比例）
    int ones = 0;
    for (int i = 0; i < 10000; i++) {
        if (ldb::BloomHash(Slice(Key(i))) & 1u) ones++;
    }
    std::printf("       10000 个哈希，最低位为 1 的有 %d 个（应接近 5000）\n", ones);
    CHECK(ones > 4600 && ones < 5400);
}

}  // namespace

int main() {
    TestNoFalseNegative();
    TestFalsePositiveRate();
    TestBitsPerKey();
    TestEdgeCases();
    TestHash();

    std::printf("\n%d / %d passed", g_total - g_failed, g_total);
    if (g_failed == 0) {
        std::printf("  === ALL OK ===\n");
        return 0;
    }
    std::printf("  [X] %d FAILED\n", g_failed);
    return 1;
}