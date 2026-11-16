// benchmark：测这个存储引擎的吞吐
//
// 五个指标：
//   ① 写入（sync=false）—— 数据进 WAL 缓冲 + MemTable，不等 fsync
//   ② 写入（sync=true） —— 每条都 fsync，模拟「不丢数据」的代价
//   ③ 随机读（存在的 key）—— MemTable + SSTable 命中
//   ④ 读不存在的 key     —— 布隆过滤器生效，省掉 pread
//   ⑤ 磁盘占用            —— 压缩效果

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <random>
#include <string>
#include <vector>

#include "db.h"
#include "slice.h"

namespace fs = std::filesystem;

namespace {

std::string Key(int i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key_%08d", i);
    return buf;
}
std::string Val(int i) { return "value_" + std::to_string(i); }

uint64_t DirSize(const std::string& dir) {
    uint64_t total = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (e.is_regular_file()) total += e.file_size();
    }
    return total;
}

double NowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

void Report(const char* name, int n, double ms) {
    const double sec = ms / 1000.0;
    std::printf("  %-28s %8d 次  %8.3f 秒  %10.0f 次/秒\n", name, n, sec, n / sec);
}

}  // namespace

int main(int argc, char** argv) {
    const std::string dir = (argc > 1) ? argv[1] : "/tmp/ldb_bench";
    const int N = (argc > 2) ? std::atoi(argv[2]) : 100000;

    std::error_code ec;
    fs::remove_all(dir, ec);

    ldb::Options opt;
    std::unique_ptr<ldb::DB> db;
    Status s = ldb::DB::Open(dir, opt, &db);
    if (!s.ok()) {
        std::fprintf(stderr, "打开失败: %s\n", s.ToString().c_str());
        return 1;
    }

    std::printf("=== mini LevelDB benchmark ===\n");
    std::printf("目录: %s\n\n", dir.c_str());

    // ---------- ① 写入（sync=false） ----------
    {
        const double t0 = NowMs();
        for (int i = 0; i < N; i++) {
            db->Put(Slice(Key(i)), Slice(Val(i)), /*sync=*/false);
        }
        Report("写入（sync=false）", N, NowMs() - t0);
    }

    // ---------- ② 写入（sync=true），数量少一点 ----------
    {
        const int M = (N < 2000) ? N : 2000;
        const double t0 = NowMs();
        for (int i = 0; i < M; i++) {
            db->Put(Slice(Key(N + i)), Slice(Val(N + i)), /*sync=*/true);
        }
        Report("写入（sync=true）", M, NowMs() - t0);
    }

    // ---------- ③ 随机读（存在） ----------
    {
        std::mt19937 rng(42);
        std::vector<int> idx(N);
        for (int i = 0; i < N; i++) idx[i] = i;
        std::shuffle(idx.begin(), idx.end(), rng);

        std::string v;
        int hit = 0;
        const double t0 = NowMs();
        for (int i = 0; i < N; i++) {
            if (db->Get(Slice(Key(idx[i])), &v).ok()) hit++;
        }
        Report("随机读（存在的 key）", N, NowMs() - t0);
        if (hit != N) std::printf("    !! 命中 %d / %d（应全中）\n", hit, N);
    }

    // ---------- ④ 读不存在的 key（布隆生效） ----------
    {
        std::string v;
        int false_hit = 0;
        const double t0 = NowMs();
        for (int i = 0; i < N; i++) {
            if (db->Get(Slice("nokey_" + std::to_string(i)), &v).ok()) false_hit++;
        }
        Report("读不存在的 key（布隆）", N, NowMs() - t0);
        if (false_hit > 0) std::printf("    !! 误命中 %d 个（应为 0）\n", false_hit);
    }

    // ---------- ⑤ 磁盘占用 ----------
    const uint64_t disk = DirSize(dir);
    const double bytes_per_entry = double(disk) / double(N);
    std::printf("\n磁盘占用: %llu 字节 (%.2f MB)\n", (unsigned long long)disk,
                disk / 1024.0 / 1024.0);
    std::printf("每条平均: %.1f 字节\n", bytes_per_entry);
    std::printf("SSTable 数: %zu\n", db->NumSSTables());

    // ---------- 收尾：flush 后看纯 SSTable 大小 ----------
    db->Flush();
    const uint64_t after = DirSize(dir);
    std::printf("flush 后: %llu 字节 (%.2f MB), %zu 个 SSTable\n",
                (unsigned long long)after, after / 1024.0 / 1024.0,
                db->NumSSTables());

    return 0;
}