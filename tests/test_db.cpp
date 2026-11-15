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

int main() {
    TestBasic();
    TestRecovery();
    TestFlushToSSTable();
    TestTombstonePersists();
    TestOverwrite();
    TestDeleteThenRewrite();
    TestManySSTables();

    std::printf("\n%d / %d passed", g_total - g_failed, g_total);
    if (g_failed == 0) {
        std::printf("  === ALL OK ===\n");
        return 0;
    }
    std::printf("  [X] %d FAILED\n", g_failed);
    return 1;
}