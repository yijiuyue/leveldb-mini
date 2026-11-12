// WAL 测试
// 重点验证三件事：
//   ① 正常写入 -> 读出，内容一致
//   ② 跨 block（> 32KB 数据）能正确读回
//   ③ 截断/损坏 -> 返回 Corruption，【不能崩】【不能读出垃圾】

#include <unistd.h>  // truncate

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "slice.h"
#include "wal.h"

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

const char* kPath = "/tmp/ldb_test.wal";

void RemoveFile() { std::remove(kPath); }

// 写 n 条记录，每条内容 "record-i"
void WriteRecords(int n) {
    wal::Writer w;
    Status s = w.Open(kPath);
    CHECK(s.ok());
    for (int i = 0; i < n; i++) {
        std::string rec = "record-" + std::to_string(i);
        s = w.AddRecord(Slice(rec));
        CHECK(s.ok());
    }
    s = w.Sync();
    CHECK(s.ok());
}

// ---------------- ① 基本读写 ----------------

void TestBasic() {
    std::printf("[1] 基本写入 / 读出\n");
    RemoveFile();
    WriteRecords(10);

    wal::Reader r;
    CHECK(r.Open(kPath).ok());

    std::string rec;
    for (int i = 0; i < 10; i++) {
        Status s = r.ReadRecord(&rec);
        CHECK(s.ok());
        CHECK(rec == ("record-" + std::to_string(i)));
    }
    // 读完应该是 NotFound（正常末尾）
    Status s = r.ReadRecord(&rec);
    CHECK(s.IsNotFound());
}

// ---------------- ② 跨 block ----------------

void TestCrossBlock() {
    std::printf("[2] 跨 block（数据 > 32KB）\n");
    RemoveFile();

    // 每条 1000 字节，写 100 条 = 100KB，跨 3 个 block
    const int kCount = 100;
    const size_t kLen = 1000;
    {
        wal::Writer w;
        CHECK(w.Open(kPath).ok());
        for (int i = 0; i < kCount; i++) {
            std::string rec(kLen, 'a' + (i % 26));
            rec[0] = static_cast<char>(i);  // 用首字节标记第几条
            CHECK(w.AddRecord(Slice(rec)).ok());
        }
        CHECK(w.Sync().ok());
    }
    {
        wal::Reader r;
        CHECK(r.Open(kPath).ok());
        std::string rec;
        int n = 0;
        while (true) {
            Status s = r.ReadRecord(&rec);
            if (s.IsNotFound()) break;
            CHECK(s.ok());
            CHECK(rec.size() == kLen);
            CHECK(static_cast<unsigned char>(rec[0]) == (n % 256));
            n++;
        }
        CHECK(n == kCount);
    }
}

// ---------------- ③ 截断 ----------------

void TestTruncated() {
    std::printf("[3] 截断的 WAL（模拟写到一半断电）\n");
    RemoveFile();
    WriteRecords(20);

    // 记录原始大小
    long full_size = 0;
    {
        FILE* f = std::fopen(kPath, "rb");
        std::fseek(f, 0, SEEK_END);
        full_size = std::ftell(f);
        std::fclose(f);
    }

    // 砍掉最后 3 字节（让最后一条记录不完整）
    CHECK(::truncate(kPath, full_size - 3) == 0);

    wal::Reader r;
    CHECK(r.Open(kPath).ok());
    std::string rec;
    int good = 0;
    bool got_corruption = false;
    while (true) {
        Status s = r.ReadRecord(&rec);
        if (s.IsNotFound()) break;
        if (s.IsCorruption()) { got_corruption = true; break; }
        CHECK(s.ok());
        good++;
    }
    // 应该读出 19 条完整记录，第 20 条报 Corruption
    CHECK(good == 19);
    CHECK(got_corruption);
}

// ---------------- ④ 翻转一个字节 ----------------

void TestCorruptedByte() {
    std::printf("[4] 中间某个字节被改坏\n");
    RemoveFile();
    WriteRecords(10);

    // 把第 3 条记录的 payload 改一个字节
    {
        FILE* f = std::fopen(kPath, "r+b");
        // 每条记录 = 8 头 + 8 数据 = 16 字节
        // 第 3 条的 payload 在 2*16 + 8 = 40
        std::fseek(f, 40, SEEK_SET);
        char c = 'X';
        std::fwrite(&c, 1, 1, f);
        std::fclose(f);
    }

    wal::Reader r;
    CHECK(r.Open(kPath).ok());
    std::string rec;
    int good = 0;
    bool got_corruption = false;
    while (true) {
        Status s = r.ReadRecord(&rec);
        if (s.IsNotFound()) break;
        if (s.IsCorruption()) { got_corruption = true; break; }
        CHECK(s.ok());
        good++;
    }
    // 前 2 条好，第 3 条坏
    CHECK(good == 2);
    CHECK(got_corruption);
}

// ---------------- ⑤ 空文件 ----------------

void TestEmpty() {
    std::printf("[5] 空文件\n");
    RemoveFile();
    {
        wal::Writer w;
        CHECK(w.Open(kPath).ok());
        CHECK(w.Close().ok());
    }
    wal::Reader r;
    CHECK(r.Open(kPath).ok());
    std::string rec;
    CHECK(r.ReadRecord(&rec).IsNotFound());
}

// ---------------- ⑥ 大记录 ----------------

void TestLargeRecord() {
    std::printf("[6] 单条大记录（接近 block 上限）\n");
    RemoveFile();
    // block 32KB，头 8 字节 -> 最大 payload 32760
    const size_t kLen = 20000;
    {
        wal::Writer w;
        CHECK(w.Open(kPath).ok());
        std::string rec(kLen, 'Z');
        CHECK(w.AddRecord(Slice(rec)).ok());
        CHECK(w.Sync().ok());
    }
    {
        wal::Reader r;
        CHECK(r.Open(kPath).ok());
        std::string rec;
        CHECK(r.ReadRecord(&rec).ok());
        CHECK(rec.size() == kLen);
        CHECK(rec[0] == 'Z' && rec[kLen - 1] == 'Z');
    }
    // 超过 block 的记录必须被拒绝
    {
        RemoveFile();
        wal::Writer w;
        CHECK(w.Open(kPath).ok());
        std::string huge(40000, 'A');
        CHECK(!w.AddRecord(Slice(huge)).ok());
    }
}

}  // namespace

int main() {
    TestBasic();
    TestCrossBlock();
    TestTruncated();
    TestCorruptedByte();
    TestEmpty();
    TestLargeRecord();

    std::printf("\n%d / %d passed", g_total - g_failed, g_total);
    if (g_failed == 0) {
        std::printf("  ✅ ALL OK\n");
        return 0;
    }
    std::printf("  ❌ %d FAILED\n", g_failed);
    return 1;
}