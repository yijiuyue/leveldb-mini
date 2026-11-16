#include <cstdio>
#include <string>
#include <atomic>
#include "util/coding.h"

struct RealNode {
    std::string key;
    std::string value;
    int height;
    std::atomic<RealNode*> next[1];
};

void Dump(const char* tag, const std::string& s) {
    std::printf("%-14s %2zu 字节 : ", tag, s.size());
    for (unsigned char c : s) std::printf("%02X ", c);
    std::printf("\n");
}

int main() {
    std::printf("=== sizeof ===\n");
    std::printf("sizeof(std::string)      = %zu\n", sizeof(std::string));
    std::printf("sizeof(std::atomic<T*>)  = %zu\n", sizeof(std::atomic<RealNode*>));
    std::printf("sizeof(RealNode)         = %zu  (含 next[1])\n", sizeof(RealNode));
    std::printf("  -> h=1: %zu, h=2: %zu, h=3: %zu\n",
        sizeof(RealNode),
        sizeof(RealNode) + sizeof(std::atomic<RealNode*>) * 1,
        sizeof(RealNode) + sizeof(std::atomic<RealNode*>) * 2);
    std::printf("  偏移: key=%zu value=%zu height=%zu next=%zu\n",
        offsetof(RealNode, key), offsetof(RealNode, value),
        offsetof(RealNode, height), offsetof(RealNode, next));
    std::printf("  SSO 容量: %zu 字节（15 字符以内不分配）\n", sizeof(std::string) - 16);

    std::printf("\n=== Put(\"ab\",\"xy\") 的 WAL payload ===\n");
    std::string payload;
    payload.push_back(1);                       // type = kRecPut
    PutLengthPrefixedSlice(&payload, Slice("ab"));
    PutLengthPrefixedSlice(&payload, Slice("xy"));
    Dump("payload", payload);

    std::printf("\n逐字段：\n");
    std::printf("  01                 type      = 1  -> 1 字节\n");
    std::printf("  02                 key_len   = 2  -> 1 字节\n");
    std::printf("  61 62              \"ab\"           -> 2 字节\n");
    std::printf("  02                 value_len = 2  -> 1 字节\n");
    std::printf("  78 79              \"xy\"           -> 2 字节\n");
    std::printf("  ------------------------------------------------\n");
    std::printf("  合计 = 1+1+2+1+2 = 7 字节（不是 9）\n");

    std::printf("\n=== 加上 WAL 头 ===\n");
    std::printf("  crc32(4) + length(4) + payload(%zu) = %zu 字节\n",
        payload.size(), payload.size() + 8);

    std::printf("\n=== Delete(\"ab\") 的 WAL payload ===\n");
    std::string del;
    del.push_back(2);                           // type = kRecDelete
    PutLengthPrefixedSlice(&del, Slice("ab"));
    PutLengthPrefixedSlice(&del, Slice());
    Dump("payload", del);
    std::printf("  合计 = 1+1+2+1+0 = 5 字节\n");
    return 0;
}