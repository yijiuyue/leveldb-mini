// DB 层测试
//
// 重点：
//   ① Put / Get / Delete 基本正确
//   ② 重启后数据还在（重放 WAL）
//   ③ MemTable 满 → 自动落 SSTable → 还能读到
//   ④ 墓碑：删除后重启还是「不存在」
//   ⑤ 覆盖写：同一个 key 写多次，读到最新的
//   ⑥ 跨多个 SSTable 的查找

#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#include "db.h"
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

namespace fs = std::filesystem;

const char* kDir = "/tmp/ldb_db_test";

void CleanDir() {
    std::error_code ec;
    fs::remove_all(kDir, ec);
}

std::unique_ptr<ldb::DB> Open(size_t buffer = 1 << 20) {
    ldb::Options opt;
    opt.write_buffer_size = buffer;
    std::unique_ptr<ldb::DB> db;
    Status s = ldb::DB::Open(kDir, opt, &db);
    CHECK(s.ok());
    return db;
}

std::string Key(int i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key_%05d", i);
    return buf;
}
std::string Val(int i) { return "val_" + std::to_string(i); }

// ---------------- ① 基本读写 ----------------

void TestBasic() {
    std::printf("[1] Put / Get / Delete\n");
    CleanDir();
    auto db = Open();
    CHECK(db->Put(Slice("a"), Slice("1")).ok());
    CHECK(db->Put(Slice("b"), Slice("2")).ok());
    CHECK(db->Put(Slice("c"), Slice("3")).ok());

    std::string v;
    CHECK(db->Get(Slice("a"), &v).ok() && v == "1");
    CHECK(db->Get(Slice("b"), &v).ok() && v == "2");
    CHECK(db->Get(Slice("nope"), &v).IsNotFound());

    CHECK(db->Delete(Slice("b")).ok());
    CHECK(db->Get(Slice("b"), &v).IsNotFound());
    CHECK(db->Get(Slice("a"), &v).ok() && v == "1");
}

// ---------------- ② 重启恢复 ----------------

void TestRecovery() {
    std::printf("[2] 重启后数据还在（重放 WAL）\n");
    CleanDir();
    {
        auto db = Open();
        for (int i = 0; i < 100; i++) {
            CHECK(db->Put(Slice(Key(i)), Slice(Val(i)), true).ok());
        }
    }
    {
        auto db = Open();
        std::string v;
        int ok = 0;
        for (int i = 0; i < 100; i++) {
            if (db->Get(Slice(Key(i)), &v).ok() && v == Val(i)) ok++;
        }
        CHECK(ok == 100);
        std::printf("       100 条全部恢复 OK\n");
    }
}

// ---------------- ③ MemTable 落盘 ----------------

void TestFlushToSSTable() {
    std::printf("[3] MemTable 满 -> 自动落 SSTable\n");
    CleanDir();
    {
        auto db = Open(8 * 1024);
        for (int i = 0; i < 500; i++) {
            CHECK(db->Put(Slice(Key(i)), Slice(Val(i))).ok());
        }
        const size_t n_sst = db->NumSSTables();
        std::printf("       500 条 -> %zu 个 SSTable\n", n_sst);
        CHECK(n_sst > 0);

        std::string v;
        int ok = 0;
        for (int i = 0; i < 500; i++) {
            if (db->Get(Slice(Key(i)), &v).ok() && v == Val(i)) ok++;
        }
        CHECK(ok == 500);
    }
    {
        auto db = Open(8 * 1024);
        std::string v;
        int ok = 0;
        for (int i = 0; i < 500; i++) {
            if (db->Get(Slice(Key(i)), &v).ok() && v == Val(i)) ok++;
        }
        CHECK(ok == 500);
        std::printf("       重启后 500 条仍全部可读 OK\n");
    }
}

// ---------------- ④ 墓碑跨重启 ----------------

void TestTombstonePersists() {
    std::printf("[4] 删除后重启，依然不存在\n");
    CleanDir();
    {
        auto db = Open();
        CHECK(db->Put(Slice("x"), Slice("1")).ok());
        CHECK(db->Put(Slice("y"), Slice("2")).ok());
        CHECK(db->Delete(Slice("x")).ok());
        CHECK(db->Flush().ok());
    }
    {
        auto db = Open();
        std::string v;
        CHECK(db->Get(Slice("x"), &v).IsNotFound());
        CHECK(db->Get(Slice("y"), &v).ok() && v == "2");
    }
}

// ---------------- ⑤ 覆盖写 ----------------

void TestOverwrite() {
    std::printf("[5] 覆盖写（读到最新的）\n");
    CleanDir();
    {
        auto db = Open();
        CHECK(db->Put(Slice("k"), Slice("v1")).ok());
        CHECK(db->Put(Slice("k"), Slice("v2")).ok());
        CHECK(db->Put(Slice("k"), Slice("v3")).ok());
        std::string v;
        CHECK(db->Get(Slice("k"), &v).ok() && v == "v3");
    }
    {
        auto db = Open();
        std::string v;
        CHECK(db->Get(Slice("k"), &v).ok() && v == "v3");
    }
}

// ---------------- ⑥ 删除后重写 ----------------

void TestDeleteThenRewrite() {
    std::printf("[6] 删除后重写同一个 key\n");
    CleanDir();
    auto db = Open();
    CHECK(db->Put(Slice("k"), Slice("v1")).ok());
    CHECK(db->Delete(Slice("k")).ok());

    std::string v;
    CHECK(db->Get(Slice("k"), &v).IsNotFound());

    CHECK(db->Put(Slice("k"), Slice("v2")).ok());
    CHECK(db->Get(Slice("k"), &v).ok() && v == "v2");
}

// ---------------- ⑦ 大量数据跨多个 SSTable ----------------

void TestManySSTables() {
    std::printf("[7] 2000 条 -> 多个 SSTable -> 全部可读\n");
    CleanDir();
    {
        auto db = Open(4 * 1024);
        for (int i = 0; i < 2000; i++) {
            CHECK(db->Put(Slice(Key(i)), Slice(Val(i))).ok());
        }
        std::printf("       2000 条 -> %zu 个 SSTable\n", db->NumSSTables());
        CHECK(db->NumSSTables() > 1);
    }
    {
        auto db = Open(4 * 1024);
        std::string v;
        int ok = 0;
        for (int i = 0; i < 2000; i++) {
            if (db->Get(Slice(Key(i)), &v).ok() && v == Val(i)) ok++;
        }
        CHECK(ok == 2000);
        std::printf("       重启后 2000 条全部可读 OK\n");
    }
}

// ---------------- ⑧ 【回归测试】打开后不写就关闭，数据不能丢 ----------------
//
// 这是修复的 bug：
//   旧实现：重放 WAL -> 立刻开新 WAL（空）-> 保留旧 WAL
//   此时崩溃 -> 重启只重放新 WAL（空）-> 数据全丢
//
// 新实现：重放 WAL -> 立刻落盘成 SSTable -> 删旧 WAL -> 开新 WAL
//   -> 任何时刻崩溃，数据要么在 SSTable，要么在唯一的 WAL

void TestReopenWithoutWrite() {
    std::printf("[8] 打开后不写就关闭（回归测试）\n");
    CleanDir();

    // ① 写 100 条（不 flush，数据只在 WAL + MemTable）
    {
        auto db = Open();
        for (int i = 0; i < 100; i++) {
            CHECK(db->Put(Slice(Key(i)), Slice(Val(i)), true).ok());
        }
    }

    // ② 打开（触发重放），什么都不写就关闭
    {
        auto db = Open();
        std::string v;
        CHECK(db->Get(Slice(Key(0)), &v).ok());
    }

    // ③ 再打开：数据必须还在（旧实现这里会丢）
    {
        auto db = Open();
        std::string v;
        int ok = 0;
        for (int i = 0; i < 100; i++) {
            if (db->Get(Slice(Key(i)), &v).ok() && v == Val(i)) ok++;
        }
        CHECK(ok == 100);
        if (ok == 100) {
            std::printf("       连续 Open 3 次，100 条一条不丢 OK\n");
        }
    }
}

// ---------------- ⑨ 【回归测试】多次 flush 后残留多个 log 也能恢复 ----------------

void TestMultipleLogs() {
    std::printf("[9] 多个残留 log 全部重放\n");
    CleanDir();

    {
        auto db = Open(4 * 1024);   // 小阈值，频繁 flush
        for (int i = 0; i < 300; i++) {
            CHECK(db->Put(Slice(Key(i)), Slice(Val(i))).ok());
        }
        std::printf("       300 条 -> %zu 个 SSTable\n", db->NumSSTables());
    }

    // 连续开三次（每次都重放 + 落盘 + 删旧 WAL）
    for (int round = 0; round < 3; round++) {
        auto db = Open(4 * 1024);
        std::string v;
        int ok = 0;
        for (int i = 0; i < 300; i++) {
            if (db->Get(Slice(Key(i)), &v).ok() && v == Val(i)) ok++;
        }
        CHECK(ok == 300);
        if (ok != 300) {
            std::printf("       第 %d 次 Open 后只读到 %d / 300 条\n", round + 1, ok);
        }
    }
    std::printf("       连续 Open 3 次，300 条一条不丢 OK\n");
}
// ---------------- ⑩ 【回归测试】坏 SSTable 必须上报，不能静默跳过 ----------------
//
// 旧实现：Get 里遇到 Table::Open 失败 / Table::Get 失败，都是 continue
//   -> 坏文件里的数据被「悄悄跳过」，可能返回更老的【过期旧值】
//   -> 表现为「数据静默倒退」，比直接报错更危险
//
// 新实现：只有 NotFound 才 continue；Corruption / IOError 一律上报

void TestCorruptSSTableReports() {
    std::printf("[10] 坏 SSTable 必须上报 Corruption\n");
    CleanDir();

    // ① 写数据并落盘
    {
        auto db = Open();
        for (int i = 0; i < 50; i++) {
            CHECK(db->Put(Slice(Key(i)), Slice(Val(i)), true).ok());
        }
        CHECK(db->Flush().ok());
        CHECK(db->NumSSTables() == 1);
    }

    // ② 找到那个 .sst，破坏它的 magic（最后一个字节）
    {
        std::string sst_path;
        for (const auto& e : fs::directory_iterator(kDir)) {
            if (e.path().extension() == ".sst") sst_path = e.path().string();
        }
        CHECK(!sst_path.empty());

        FILE* f = std::fopen(sst_path.c_str(), "r+b");
        CHECK(f != nullptr);
        std::fseek(f, 0, SEEK_END);
        const long sz = std::ftell(f);
        std::fseek(f, sz - 1, SEEK_SET);
        unsigned char c = 0;
        std::fread(&c, 1, 1, f);
        c ^= 0xFF;
        std::fseek(f, sz - 1, SEEK_SET);
        std::fwrite(&c, 1, 1, f);
        std::fclose(f);
        std::printf("       已破坏 %s 的 magic\n", sst_path.c_str());
    }

    // ③ 重新打开：Get 一个【该文件里存在】的 key，必须返回 Corruption
    {
        auto db = Open();
        std::string v;
        Status s = db->Get(Slice(Key(7)), &v);
        CHECK(!s.ok());
        CHECK(s.IsCorruption());
        if (s.IsCorruption()) {
            std::printf("       Get 返回 Corruption（正确，不再静默跳过）\n");
        } else {
            std::printf("       Get 返回 %s（错误！应该是 Corruption）\n",
                        s.ToString().c_str());
        }
    }
}
int main() {
    TestBasic();
    TestRecovery();
    TestFlushToSSTable();
    TestTombstonePersists();
    TestOverwrite();
    TestDeleteThenRewrite();
    TestManySSTables();
    TestReopenWithoutWrite();
    TestMultipleLogs();
    TestCorruptSSTableReports();

    std::printf("\n%d / %d passed", g_total - g_failed, g_total);
    if (g_failed == 0) {
        std::printf("  === ALL OK ===\n");
        return 0;
    }
    std::printf("  [X] %d FAILED\n", g_failed);
    return 1;
}