// kill -9 崩溃恢复测试的「被测程序」
//
// 两个模式：
//   writer <dir>   不停写入，每条 Put(sync=true) 后打印序号
//                  → 打印出来的一行 = 「这条已经 fsync 落盘」
//   verify <dir>  打开数据库，检查数据完整性
//
// 用法（由 run_crash_test.sh 驱动）：
//   ./crash_test writer /tmp/db &
//   kill -9 $!
//   ./crash_test verify /tmp/db

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "db.h"
#include "slice.h"

namespace {

std::string Key(int i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key_%08d", i);
    return buf;
}

std::string Val(int i) { return "value_" + std::to_string(i); }

// writer：无限写入，每条约 fsync
int RunWriter(const std::string& dir) {
    ldb::Options opt;
    opt.write_buffer_size = 64 * 1024;   // 64KB 就落盘，制造频繁的 flush

    std::unique_ptr<ldb::DB> db;
    Status s = ldb::DB::Open(dir, opt, &db);
    if (!s.ok()) {
        std::fprintf(stderr, "open failed: %s\n", s.ToString().c_str());
        return 2;
    }

    for (int i = 0; i < 200000; i++) {
        s = db->Put(Slice(Key(i)), Slice(Val(i)), /*sync=*/true);
        if (!s.ok()) {
            std::fprintf(stderr, "put %d failed: %s\n", i, s.ToString().c_str());
            return 3;
        }
        // 【关键】打印 + 立刻 flush，让父进程能马上看到「这条已确认」
        std::printf("%d\n", i);
        std::fflush(stdout);
    }
    return 0;
}

// verify：检查 0..N 是否连续存在（不允许有洞）
int RunVerify(const std::string& dir) {
    ldb::Options opt;
    opt.write_buffer_size = 64 * 1024;

    std::unique_ptr<ldb::DB> db;
    Status s = ldb::DB::Open(dir, opt, &db);
    if (!s.ok()) {
        std::printf("RECOVERED -1\n");
        std::fprintf(stderr, "open failed: %s\n", s.ToString().c_str());
        return 2;
    }

    // ① 找最长的连续前缀 [0, n)
    int n = 0;
    std::string v;
    while (n < 200000) {
        if (db->Get(Slice(Key(n)), &v).ok() && v == Val(n)) {
            n++;
        } else {
            break;
        }
    }

    // ② 检查第一个洞之后是否还有数据（有 = 数据错乱，不只是「少了几条」）
    int gap_tail = 0;
    for (int i = n + 1; i < n + 200 && i < 200000; i++) {
        if (db->Get(Slice(Key(i)), &v).ok() && v == Val(i)) gap_tail++;
    }

    std::printf("RECOVERED %d\n", n);
    std::printf("GAP_TAIL %d\n", gap_tail);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <writer|verify> <dir>\n", argv[0]);
        return 1;
    }
    const std::string mode = argv[1];
    const std::string dir = argv[2];

    if (mode == "writer") return RunWriter(dir);
    if (mode == "verify") return RunVerify(dir);

    std::fprintf(stderr, "unknown mode: %s\n", mode.c_str());
    return 1;
}