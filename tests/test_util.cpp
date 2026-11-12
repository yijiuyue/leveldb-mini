// 工具层测试：varint / fixed / CRC32C
// 不依赖 gtest，自己写断言 —— 目的是「clone 下来就能 make && ./test_util」

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "slice.h"
#include "util/coding.h"
#include "util/crc32c.h"

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

// ---------------- varint ----------------

static void TestVarint() {
    std::printf("[1] varint\n");

    // 边界值
    const uint32_t cases[] = {0,          1,          127,        128,
                              255,        300,        16383,      16384,
                              0x7fffffff, 0xffffffff};
    for (uint32_t v : cases) {
        std::string buf;
        PutVarint32(&buf, v);
        Slice input(buf);
        uint32_t got = 0;
        CHECK(GetVarint32(&input, &got));
        CHECK(got == v);
        CHECK(input.empty());
    }

    // 编码长度：小数字应该只占 1 字节
    {
        std::string buf;
        PutVarint32(&buf, 127);
        CHECK(buf.size() == 1);
    }
    {
        std::string buf;
        PutVarint32(&buf, 300);
        CHECK(buf.size() == 2);
    }

    // 【关键】解码不能越界：给一个不完整的 varint
    {
        std::string buf;
        PutVarint32(&buf, 0x7fffffff);
        Slice truncated(buf.data(), buf.size() - 1);  // 砍掉最后一个字节
        uint32_t got = 0;
        CHECK(!GetVarint32(&truncated, &got));  // 必须返回 false，不能崩
    }

    // 空输入
    {
        Slice empty_slice;
        uint32_t got = 0;
        CHECK(!GetVarint32(&empty_slice, &got));
    }
}

// ---------------- fixed ----------------

static void TestFixed() {
    std::printf("[2] fixed32 / fixed64\n");

    {
        std::string buf;
        PutFixed32(&buf, 0x12345678u);
        CHECK(buf.size() == 4);
        Slice input(buf);
        uint32_t v = 0;
        CHECK(GetFixed32(&input, &v));
        CHECK(v == 0x12345678u);
    }
    {
        std::string buf;
        PutFixed64(&buf, 0x123456789abcdef0ULL);
        CHECK(buf.size() == 8);
        Slice input(buf);
        uint64_t v = 0;
        CHECK(GetFixed64(&input, &v));
        CHECK(v == 0x123456789abcdef0ULL);
    }
    // 数据不足
    {
        Slice s("abc", 3);
        uint32_t v = 0;
        CHECK(!GetFixed32(&s, &v));
    }
}

// ---------------- 长度前缀 ----------------

static void TestLengthPrefixed() {
    std::printf("[3] length-prefixed slice\n");

    std::string buf;
    PutLengthPrefixedSlice(&buf, Slice("hello"));
    PutLengthPrefixedSlice(&buf, Slice(""));
    PutLengthPrefixedSlice(&buf, Slice("world!"));

    Slice input(buf);
    Slice out;

    CHECK(GetLengthPrefixedSlice(&input, &out));
    CHECK(out.ToString() == "hello");

    CHECK(GetLengthPrefixedSlice(&input, &out));
    CHECK(out.ToString() == "");

    CHECK(GetLengthPrefixedSlice(&input, &out));
    CHECK(out.ToString() == "world!");

    CHECK(input.empty());

    // 长度说 100 但只有 3 字节 -> 必须失败
    std::string bad;
    PutVarint32(&bad, 100);
    bad.append("abc");
    Slice bad_input(bad);
    CHECK(!GetLengthPrefixedSlice(&bad_input, &out));
}

// ---------------- CRC32C ----------------

static void TestCRC() {
    std::printf("[4] crc32c\n");

    // 标准测试向量："123456789" 的 CRC32C = 0xE3069283
    CHECK(crc32c::Value("123456789", 9) == 0xE3069283u);

    // 空输入的 CRC = 0
    CHECK(crc32c::Value("", 0) == 0u);

    // 分段计算结果一致（Extend 的用途）
    {
        const char* s = "hello world";
        uint32_t whole = crc32c::Value(s, 11);
        uint32_t part1 = crc32c::Extend(0, s, 5);
        uint32_t part2 = crc32c::Extend(part1, s + 5, 6);
        CHECK(whole == part2);
    }

    // 改动一个字节，CRC 必须变化
    {
        std::string a = "abcdefgh";
        std::string b = "abcdefgX";
        CHECK(crc32c::Value(a.data(), a.size()) != crc32c::Value(b.data(), b.size()));
    }
}

// ---------------- Slice ----------------

static void TestSlice() {
    std::printf("[5] slice\n");

    Slice a("abc");
    Slice b("abd");
    CHECK(a.Compare(b) < 0);
    CHECK(b.Compare(a) > 0);
    CHECK(a.Compare(a) == 0);

    Slice longer("abcd");
    CHECK(a.Compare(longer) < 0);  // 前缀相同，短的小

    {
        Slice s("abc");
        s.RemovePrefix(1);
        CHECK(s.ToString() == "bc");
    }
    CHECK(a.empty() == false);
    CHECK(Slice().empty() == true);
}

int main() {
    TestVarint();
    TestFixed();
    TestLengthPrefixed();
    TestCRC();
    TestSlice();

    std::printf("\n%d / %d passed", g_total - g_failed, g_total);
    if (g_failed == 0) {
        std::printf("  ✅ ALL OK\n");
        return 0;
    }
    std::printf("  ❌ %d FAILED\n", g_failed);
    return 1;
}