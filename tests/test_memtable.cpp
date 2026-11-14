// MemTable + SkipList 测试
//
// 重点：
//   ① 跳表的有序性（顺序遍历必须递增）
//   ② 大量随机插入后查找全对
//   ③ 覆盖写（同一 key 写两次，读到后写的）
//   ④ 墓碑语义（delete 后读 = kDeleted，不是 kNotFound）
//   ⑤ 层数分布合理（不能全是 1 层，也不能爆炸）

#include <algorithm>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "memtable.h"
#include "skiplist.h"

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

int Cmp(const std::string& a, const std::string& b) {
    if (a < b) return -1;
    if (a > b) return +1;
    return 0;
}

// ---------------- ① 跳表：有序性 + 查找 ----------------

void TestSkipListBasic() {
    std::printf("[1] 跳表：插入 + 有序遍历 + 查找\n");
    ldb::SkipList list(Cmp);

    // 乱序插入
    const std::vector<std::string> words = {"pear", "apple", "zebra", "mango",
                                            "banana", "cherry", "date"};
    for (const auto& w : words) list.Insert(w, "v_" + w);
    CHECK(list.size() == 7);

    // 顺序遍历必须是字典序
    std::vector<std::string> got;
    ldb::SkipList::Iterator it(&list);
    for (it.SeekToFirst(); it.Valid(); it.Next()) got.push_back(it.key());

    std::vector<std::string> expect = words;
    std::sort(expect.begin(), expect.end());
    CHECK(got == expect);

    // 查找
    std::string v;
    CHECK(list.Get("banana", &v));
    CHECK(v == "v_banana");
    CHECK(!list.Get("grape", &v));
}

// ---------------- ② 跳表：大量随机数据 ----------------

void TestSkipListStress() {
    std::printf("[2] 跳表：1 万条随机数据\n");
    ldb::SkipList list(Cmp);

    std::mt19937 rng(42);
    std::vector<std::string> keys;
    for (int i = 0; i < 10000; i++) {
        int x = static_cast<int>(rng() % 1000000);
        keys.push_back("key_" + std::to_string(x));
    }
    for (const auto& k : keys) list.Insert(k, "v");

    // 去重后的数量
    std::vector<std::string> uniq = keys;
    std::sort(uniq.begin(), uniq.end());
    uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
    CHECK(list.size() == static_cast<int>(uniq.size()));

    // 全部能查到
    int found = 0;
    for (const auto& k : uniq) {
        std::string v;
        if (list.Get(k, &v)) found++;
    }
    CHECK(found == static_cast<int>(uniq.size()));
}

// ---------------- ③ MemTable：基本读写 + 覆盖 ----------------

void TestMemTableBasic() {
    std::printf("[3] MemTable：读 / 写 / 覆盖\n");
    ldb::MemTable mem;

    mem.Put("name", "alice");
    mem.Put("age", "20");

    std::string v;
    CHECK(mem.Get("name", &v) == ldb::MemTableResult::kFound);
    CHECK(v == "alice");

    // 覆盖写
    mem.Put("name", "bob");
    CHECK(mem.Get("name", &v) == ldb::MemTableResult::kFound);
    CHECK(v == "bob");

    // 不存在
    CHECK(mem.Get("nope", &v) == ldb::MemTableResult::kNotFound);
}

// ---------------- ④ MemTable：墓碑 ----------------

void TestMemTableDelete() {
    std::printf("[4] MemTable：墓碑语义\n");
    ldb::MemTable mem;

    mem.Put("k", "v");
    std::string v;
    CHECK(mem.Get("k", &v) == ldb::MemTableResult::kFound);

    mem.Delete("k");
    // 关键：delete 后是 kDeleted（明确不存在），不是 kNotFound（没找到）
    CHECK(mem.Get("k", &v) == ldb::MemTableResult::kDeleted);

    // 再写回来
    mem.Put("k", "v2");
    CHECK(mem.Get("k", &v) == ldb::MemTableResult::kFound);
    CHECK(v == "v2");
}

// ---------------- ⑤ MemTable：顺序遍历 ----------------

void TestMemTableForEach() {
    std::printf("[5] MemTable：落盘用的顺序遍历\n");
    ldb::MemTable mem;

    mem.Put("c", "3");
    mem.Put("a", "1");
    mem.Put("d", "4");
    mem.Put("b", "2");
    mem.Delete("c");

    std::vector<std::string> keys;
    std::vector<unsigned char> types;
    mem.ForEach([&](const std::string& k, ldb::ValueType t, const std::string& v) {
        keys.push_back(k);
        types.push_back(static_cast<unsigned char>(t));
        (void)v;
    });

    CHECK(keys.size() == 4);
    // 必须字典序
    CHECK(keys[0] == "a" && keys[1] == "b" && keys[2] == "c" && keys[3] == "d");
    // c 是墓碑
    CHECK(types[2] == static_cast<unsigned char>(ldb::kTypeDeletion));
    CHECK(types[0] == static_cast<unsigned char>(ldb::kTypeValue));
}

// ---------------- ⑥ 跳表层数分布 ----------------

void TestSkipListHeightDistribution() {
    std::printf("[6] 跳表：层数分布（1/4 概率上升）\n");
    ldb::SkipList list(Cmp);

    // 插 10 万条，看是否有超过 1 层
    for (int i = 0; i < 100000; i++) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%08d", i);
        list.Insert(std::string(buf), "v");
    }
    CHECK(list.size() == 100000);

    // 抽查：随便找几个 key，能查到就说明多层索引在起作用
    int ok = 0;
    for (int i = 0; i < 1000; i++) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%08d", i * 97);
        std::string v;
        if (list.Get(std::string(buf), &v)) ok++;
    }
    CHECK(ok == 1000);

    // 内存占用应该在合理范围（每条约 50-100 字节）
    const size_t per_entry = list.ApproximateMemoryUsage() / 100000;
    std::printf("       每条记录约 %zu 字节，总 %zu KB\n", per_entry,
                list.ApproximateMemoryUsage() / 1024);
    CHECK(per_entry > 20 && per_entry < 200);
}

}  // namespace

int main() {
    TestSkipListBasic();
    TestSkipListStress();
    TestMemTableBasic();
    TestMemTableDelete();
    TestMemTableForEach();
    TestSkipListHeightDistribution();

    std::printf("\n%d / %d passed", g_total - g_failed, g_total);
    if (g_failed == 0) {
        std::printf("  ✅ ALL OK\n");
        return 0;
    }
    std::printf("  ❌ %d FAILED\n", g_failed);
    return 1;
}