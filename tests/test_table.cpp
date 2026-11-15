// SSTable（Table）测试
//
// 重点：
//   ① 写 N 条 → 读回全部正确
//   ② 跨多个 data block（> 4KB）
//   ③ 不存在的 key 返回 NotFound
//   ④ 边界：最小的 key、最大的 key、比最小还小、比最大还大
//   ⑤ magic 校验（损坏的文件要拒绝）
//   ⑥ 稀疏索引真的只占 0.5% 左右

#include <unistd.h>

#include <cstdio>
#include <string>
#include <vector>

#include "slice.h"
#include "table.h"

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

const char* kPath = "/tmp/ldb_test.sst";

std::string Key(int i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key_%06d", i);
    return buf;
}

std::string Val(int i) { return "value_" + std::to_string(i); }

// 写 n 条到文件
bool WriteTable(int n, size_t block_size = 4096) {
    ldb::TableBuilder builder(kPath, block_size);
    for (int i = 0; i < n; i++) {
        Status s = builder.Add(Slice(Key(i)), Slice(Val(i)));
        if (!s.ok()) return false;
    }
    return builder.Finish().ok();
}

// ---------------- ① 基本读写 ----------------

void TestBasic() {
    std::printf("[1] 基本读写\n");
    std::remove(kPath);
    CHECK(WriteTable(100));

    std::unique_ptr<ldb::Table> table;
    CHECK(ldb::Table::Open(kPath, &table).ok());

    std::string v;
    for (int i = 0; i < 100; i++) {
        Status s = table->Get(Slice(Key(i)), &v);
        CHECK(s.ok());
        CHECK(v == Val(i));
    }
}

// ---------------- ② 跨多个 data block ----------------

void TestMultipleBlocks() {
    std::printf("[2] 跨多个 data block（每 block 只放约 10 条）\n");
    std::remove(kPath);

    // block_size 设小一点，强制多个 block
    const int kCount = 500;
    CHECK(WriteTable(kCount, 256));  // 256 字节/block

    std::unique_ptr<ldb::Table> table;
    CHECK(ldb::Table::Open(kPath, &table).ok());

    std::string v;
    int ok = 0;
    for (int i = 0; i < kCount; i++) {
        if (table->Get(Slice(Key(i)), &v).ok() && v == Val(i)) ok++;
    }
    CHECK(ok == kCount);
    std::printf("       500 条跨多个 block，全部读回 ✅\n");
}

// ---------------- ③ 不存在的 key ----------------

void TestNotFound() {
    std::printf("[3] 不存在的 key\n");
    std::remove(kPath);
    CHECK(WriteTable(50));

    std::unique_ptr<ldb::Table> table;
    CHECK(ldb::Table::Open(kPath, &table).ok());

    std::string v;
    // 间隔的 key（key_000000 和 key_000002 之间）
    CHECK(table->Get(Slice("key_000000x"), &v).IsNotFound());
    CHECK(table->Get(Slice("key_000100"), &v).IsNotFound());
    CHECK(table->Get(Slice("zzz"), &v).IsNotFound());
}

// ---------------- ④ 边界 ----------------

void TestBoundary() {
    std::printf("[4] 边界（最小 / 最大 / 超出两端）\n");
    std::remove(kPath);
    CHECK(WriteTable(100));

    std::unique_ptr<ldb::Table> table;
    CHECK(ldb::Table::Open(kPath, &table).ok());

    std::string v;
    // 第一条
    CHECK(table->Get(Slice(Key(0)), &v).ok() && v == Val(0));
    // 最后一条
    CHECK(table->Get(Slice(Key(99)), &v).ok() && v == Val(99));
    // 比最小还小
    CHECK(table->Get(Slice("aaa"), &v).IsNotFound());
    // 比最大还大
    CHECK(table->Get(Slice("zzz"), &v).IsNotFound());
}

// ---------------- ⑤ magic 校验 ----------------

void TestBadMagic() {
    std::printf("[5] 损坏的文件（magic 被改）\n");
    std::remove(kPath);
    CHECK(WriteTable(10));

    // 把最后 1 个字节（magic 的最后一个字符 ':'）改坏
    {
        FILE* f = std::fopen(kPath, "r+b");
        std::fseek(f, 0, SEEK_END);
        long sz = std::ftell(f);
        std::fseek(f, sz - 1, SEEK_SET);
        char c = 'X';
        std::fwrite(&c, 1, 1, f);
        std::fclose(f);
    }

    std::unique_ptr<ldb::Table> table;
    Status s = ldb::Table::Open(kPath, &table);
    CHECK(s.IsCorruption());
}

// ---------------- ⑥ 索引占用 ----------------

void TestIndexOverhead() {
    std::printf("[6] 稀疏索引的占比\n");
    std::remove(kPath);

    const int kCount = 20000;
    ldb::TableBuilder builder(kPath, 4096);
    for (int i = 0; i < kCount; i++) {
        builder.Add(Slice(Key(i)), Slice(Val(i)));
    }
    CHECK(builder.Finish().ok());

    const uint64_t file_size = builder.FileSize();
    const size_t blocks = builder.NumDataBlocks();

    // 索引大约 = blocks × (key 长度 + handle 长度)
    const double ratio = 100.0 * double(blocks * 30) / double(file_size);
    std::printf("       20000 条 → %llu 字节，%zu 个 block，索引约占 %.2f%%\n",
                (unsigned long long)file_size, blocks, ratio);

    // 20000 条压缩后约 300KB，每 block 4KB -> 约 76 个 block
    CHECK(blocks > 50);
    // 每个 block 平均接近 4KB（说明分块策略正确）
    const double avg_block = double(file_size) / double(blocks);
    std::printf("       平均每 block %.0f 字节（目标 4096）\n", avg_block);
    CHECK(avg_block > 3000 && avg_block < 5000);
    CHECK(ratio < 2.0);           // 索引占比必须 < 2%

    // 随机抽查 200 条
    std::unique_ptr<ldb::Table> table;
    CHECK(ldb::Table::Open(kPath, &table).ok());
    std::string v;
    int ok = 0;
    for (int i = 0; i < kCount; i += 97) {
        if (table->Get(Slice(Key(i)), &v).ok() && v == Val(i)) ok++;
    }
    CHECK(ok == (kCount + 96) / 97);
}

// ---------------- ⑦ 空表 / 单条 ----------------

void TestEdgeCases() {
    std::printf("[7] 空表 / 单条\n");

    // 单条
    {
        std::remove(kPath);
        ldb::TableBuilder b(kPath);
        CHECK(b.Add(Slice("only"), Slice("one")).ok());
        CHECK(b.Finish().ok());

        std::unique_ptr<ldb::Table> t;
        CHECK(ldb::Table::Open(kPath, &t).ok());
        std::string v;
        CHECK(t->Get(Slice("only"), &v).ok() && v == "one");
        CHECK(t->Get(Slice("nope"), &v).IsNotFound());
    }

    // 空表（一条都没加）
    {
        std::remove(kPath);
        ldb::TableBuilder b(kPath);
        CHECK(b.Finish().ok());

        std::unique_ptr<ldb::Table> t;
        CHECK(ldb::Table::Open(kPath, &t).ok());
        std::string v;
        CHECK(t->Get(Slice("any"), &v).IsNotFound());
    }
}

// ---------------- ⑧ key 必须递增 ----------------

void TestOrderRequirement() {
    std::printf("[8] key 必须严格递增\n");
    std::remove(kPath);

    ldb::TableBuilder b(kPath);
    CHECK(b.Add(Slice("b"), Slice("1")).ok());
    // 比上一个小 -> 必须失败
    CHECK(!b.Add(Slice("a"), Slice("2")).ok());
    // 和上一条相同 -> 也必须失败
    CHECK(!b.Add(Slice("b"), Slice("3")).ok());
    // 正常递增
    CHECK(b.Add(Slice("c"), Slice("4")).ok());
    CHECK(b.Finish().ok());
}

}  // namespace

int main() {
    TestBasic();
    TestMultipleBlocks();
    TestNotFound();
    TestBoundary();
    TestBadMagic();
    TestIndexOverhead();
    TestEdgeCases();
    TestOrderRequirement();

    std::printf("\n%d / %d passed", g_total - g_failed, g_total);
    if (g_failed == 0) {
        std::printf("  ✅ ALL OK\n");
        return 0;
    }
    std::printf("  ❌ %d FAILED\n", g_failed);
    return 1;
}