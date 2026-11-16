// db_demo：一个最简命令行工具，用来手动试这个存储引擎
//
// 用法：
//   ./db_demo <dir> put <key> <value>
//   ./db_demo <dir> get <key>
//   ./db_demo <dir> del <key>
//   ./db_demo <dir> fill <n>       批量写 n 条（key_000000 形式）
//   ./db_demo <dir> check <n>      检查 n 条是否都在
//   ./db_demo <dir> stat           显示状态

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "db.h"
#include "slice.h"

namespace fs = std::filesystem;

namespace {

std::string Key(int i) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "key_%06d", i);
    return buf;
}

uint64_t DirSize(const std::string& dir) {
    uint64_t total = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        if (ec) break;
        if (e.is_regular_file()) total += e.file_size();
    }
    return total;
}

void Usage(const char* prog) {
    std::fprintf(stderr,
                 "用法:\n"
                 "  %s <dir> put <key> <value>\n"
                 "  %s <dir> get <key>\n"
                 "  %s <dir> del <key>\n"
                 "  %s <dir> fill <n>\n"
                 "  %s <dir> check <n>\n"
                 "  %s <dir> stat\n",
                 prog, prog, prog, prog, prog, prog);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        Usage(argv[0]);
        return 1;
    }

    const std::string dir = argv[1];
    const std::string cmd = argv[2];

    ldb::Options opt;
    std::unique_ptr<ldb::DB> db;
    Status s = ldb::DB::Open(dir, opt, &db);
    if (!s.ok()) {
        std::fprintf(stderr, "打开失败: %s\n", s.ToString().c_str());
        return 1;
    }

    // ---------- put ----------
    if (cmd == "put") {
        if (argc < 5) { Usage(argv[0]); return 1; }
        s = db->Put(Slice(argv[3]), Slice(argv[4]), /*sync=*/true);
        if (!s.ok()) {
            std::fprintf(stderr, "写入失败: %s\n", s.ToString().c_str());
            return 1;
        }
        std::printf("OK\n");
        return 0;
    }

    // ---------- get ----------
    if (cmd == "get") {
        if (argc < 4) { Usage(argv[0]); return 1; }
        std::string v;
        s = db->Get(Slice(argv[3]), &v);
        if (s.IsNotFound()) {
            std::printf("(not found)\n");
            return 2;
        }
        if (!s.ok()) {
            std::fprintf(stderr, "读取失败: %s\n", s.ToString().c_str());
            return 1;
        }
        std::printf("%s\n", v.c_str());
        return 0;
    }

    // ---------- del ----------
    if (cmd == "del") {
        if (argc < 4) { Usage(argv[0]); return 1; }
        s = db->Delete(Slice(argv[3]), /*sync=*/true);
        if (!s.ok()) {
            std::fprintf(stderr, "删除失败: %s\n", s.ToString().c_str());
            return 1;
        }
        std::printf("OK\n");
        return 0;
    }

    // ---------- fill ----------
    if (cmd == "fill") {
        if (argc < 4) { Usage(argv[0]); return 1; }
        const int n = std::atoi(argv[3]);
        for (int i = 0; i < n; i++) {
            s = db->Put(Slice(Key(i)), Slice("value_" + std::to_string(i)));
            if (!s.ok()) {
                std::fprintf(stderr, "第 %d 条失败: %s\n", i, s.ToString().c_str());
                return 1;
            }
        }
        std::printf("写入 %d 条，%zu 个 SSTable，%llu 字节\n", n, db->NumSSTables(),
                    (unsigned long long)DirSize(dir));
        return 0;
    }

    // ---------- check ----------
    if (cmd == "check") {
        if (argc < 4) { Usage(argv[0]); return 1; }
        const int n = std::atoi(argv[3]);
        int ok = 0;
        std::string v;
        for (int i = 0; i < n; i++) {
            if (db->Get(Slice(Key(i)), &v).ok() &&
                v == "value_" + std::to_string(i)) {
                ok++;
            }
        }
        std::printf("%d / %d 条正确\n", ok, n);
        return ok == n ? 0 : 1;
    }

    // ---------- stat ----------
    if (cmd == "stat") {
        std::printf("目录        : %s\n", dir.c_str());
        std::printf("SSTable 数  : %zu\n", db->NumSSTables());
        std::printf("MemTable 大小: %zu 字节\n", db->MemTableSize());
        std::printf("磁盘占用    : %llu 字节\n", (unsigned long long)DirSize(dir));

        // 列一下文件
        std::error_code ec;
        int n_log = 0, n_sst = 0;
        for (const auto& e : fs::directory_iterator(dir, ec)) {
            if (ec) break;
            const auto ext = e.path().extension().string();
            if (ext == ".log") n_log++;
            else if (ext == ".sst") n_sst++;
        }
        std::printf("文件        : %d 个 .log, %d 个 .sst\n", n_log, n_sst);
        return 0;
    }

    Usage(argv[0]);
    return 1;
}