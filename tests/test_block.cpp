// Block 测试
//
// 重点：
//   ① 编码/解码一致（写进去的能原样读出来）
//   ② 前缀压缩真的生效（相同前缀的 key 占用更少）
//   ③ restart 边界（第 16、32 条是 restart 点）
//   ④ 找不到的 key 返回 false
//   ⑤ 大量数据 + 随机查找全部命中

#include <cstdio>
#include <string>
#include <vector>

#include "block.h"
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

// ---------------- ① 基本编码/解码 ----------------

void TestBasic() {
    std::printf("[1] 基本编码 / 解码\n");
    ldb::BlockBuilder builder;

    builder.Add(Slice("apple"), Slice("1"));
    builder.Add(Slice("banana"), Slice("2"));
    builder.Add(Slice("cherry"), Slice("3"));

    Slice contents = builder.Finish();
    ldb::Block block(contents);

    CHECK(block.NumEntries() == 3);

    std::string v;
    CHECK(block.Get(Slice("apple"), &v) && v == "1");
    CHECK(block.Get(Slice("banana"), &v) && v == "2");
    CHECK(block.Get(Slice("cherry"), &v) && v == "3");

    // 不存在的 key
    CHECK(!block.Get(Slice("avocado"), &v));
    CHECK(!block.Get(Slice("aaa"), &v));      // 比最小还小
    CHECK(!block.Get(Slice("zebra"), &v));    // 比最大还大
}

// ---------------- ② 前缀压缩生效 ----------------

void TestPrefixCompression() {
    std::printf("[2] 前缀压缩真的省空间\n");

    // 10 条共享前缀的 key
    std::vector<std::string> keys;
    for (int i = 0; i < 10; i++) {
        keys.push_back("user:profile:name:" + std::to_string(1000 + i));
    }

    // 压缩版
    ldb::BlockBuilder compressed;
    for (const auto& k : keys) compressed.Add(Slice(k), Slice("v"));
    const size_t compressed_size = compressed.Finish().size();

    // 不压缩（restart_interval = 1 -> 每条都是 restart，shared 永远 0）
    ldb::BlockBuilder plain(1);
    for (const auto& k : keys) plain.Add(Slice(k), Slice("v"));
    const size_t plain_size = plain.Finish().size();

    std::printf("       压缩: %zu 字节 / 不压缩: %zu 字节（省 %.0f%%）\n",
                compressed_size, plain_size,
                100.0 * (1.0 - double(compressed_size) / double(plain_size)));

    CHECK(compressed_size < plain_size);

    // 两个都必须能正确读回
    ldb::Block b1(compressed.Finish());
    ldb::Block b2(plain.Finish());
    std::string v;
    for (const auto& k : keys) {
        CHECK(b1.Get(Slice(k), &v) && v == "v");
        CHECK(b2.Get(Slice(k), &v) && v == "v");
    }
}

// ---------------- ③ restart 边界 ----------------

void TestRestartBoundary() {
    std::printf("[3] restart 边界（每 16 条一个 restart）\n");
    ldb::BlockBuilder builder(16);

    // 50 条：key 是固定宽度数字，前缀几乎全同
    std::vector<std::string> keys;
    for (int i = 0; i < 50; i++) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%08d", i);
        keys.push_back(buf);
        builder.Add(Slice(keys.back()), Slice("v" + std::to_string(i)));
    }

    Slice contents = builder.Finish();
    ldb::Block block(contents);

    CHECK(block.NumEntries() == 50);
    // 50 条 / 每 16 条一个 restart -> ceil(50/16) = 4 个 restart
    CHECK(block.NumRestarts() == 4);

    // 每条都能读回（尤其跨 restart 边界的那几条）
    std::string v;
    for (int i = 0; i < 50; i++) {
        CHECK(block.Get(Slice(keys[i]), &v));
        CHECK(v == "v" + std::to_string(i));
    }
}

// ---------------- ④ 大量数据 ----------------

void TestLargeBlock() {
    std::printf("[4] 5000 条 + 随机查找\n");
    ldb::BlockBuilder builder;

    std::vector<std::string> keys;
    for (int i = 0; i < 5000; i++) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "key_%06d", i * 3);  // 有间隔，便于测「找不到」
        keys.push_back(buf);
        builder.Add(Slice(keys.back()), Slice(std::to_string(i)));
    }

    Slice contents = builder.Finish();
    ldb::Block block(contents);
    CHECK(block.NumEntries() == 5000);
    std::printf("       5000 条占 %zu 字节（每条 %.1f 字节）\n", contents.size(),
                double(contents.size()) / 5000.0);

    // 全部命中
    std::string v;
    for (int i = 0; i < 5000; i++) {
        CHECK(block.Get(Slice(keys[i]), &v));
        CHECK(v == std::to_string(i));
    }

    // 不存在的 key（间隔里的）
    int false_hits = 0;
    for (int i = 0; i < 5000; i++) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "key_%06d", i * 3 + 1);
        if (block.Get(Slice(buf), &v)) false_hits++;
    }
    CHECK(false_hits == 0);
}

// ---------------- ⑤ 单条 / 空 -----------------

void TestEdgeCases() {
    std::printf("[5] 单条 / 空 block\n");

    // 单条
    {
        ldb::BlockBuilder b;
        b.Add(Slice("only"), Slice("one"));
        ldb::Block block(b.Finish());
        CHECK(block.NumEntries() == 1);
        CHECK(block.NumRestarts() == 1);
        std::string v;
        CHECK(block.Get(Slice("only"), &v) && v == "one");
        CHECK(!block.Get(Slice("only2"), &v));
    }

    // 空 value
    {
        ldb::BlockBuilder b;
        b.Add(Slice("k"), Slice(""));
        ldb::Block block(b.Finish());
        std::string v = "xxx";
        CHECK(block.Get(Slice("k"), &v));
        CHECK(v.empty());
    }
}

}  // namespace

int main() {
    TestBasic();
    TestPrefixCompression();
    TestRestartBoundary();
    TestLargeBlock();
    TestEdgeCases();

    std::printf("\n%d / %d passed", g_total - g_failed, g_total);
    if (g_failed == 0) {
        std::printf("  ✅ ALL OK\n");
        return 0;
    }
    std::printf("  ❌ %d FAILED\n", g_failed);
    return 1;
}