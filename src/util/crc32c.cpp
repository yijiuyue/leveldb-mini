#include "util/crc32c.h"

namespace crc32c {

namespace {

// CRC32C 的多项式（Castagnoli），反射形式
constexpr uint32_t kPoly = 0x82f63b78u;

// 256 项查找表：t[i] = 「字节 i 经过 8 轮逐位运算」的结果
// 用 constexpr 在【编译期】算好，运行时零开销
struct Table {
    uint32_t t[256];
    constexpr Table() : t() {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t crc = i;
            for (int j = 0; j < 8; j++) {
                // 最低位是 1 -> 右移后异或多项式（模 2 的减法）
                // 最低位是 0 -> 只右移
                crc = (crc & 1u) ? ((crc >> 1) ^ kPoly) : (crc >> 1);
            }
            t[i] = crc;
        }
    }
};

constexpr Table kTable{};

}  // namespace

// ---------------- 生产版：查表，一次一字节 ----------------
//
// 为什么能一次处理一字节：
//   一个字节 8 位的处理结果只取决于「这个字节」和「进来时 crc 的低 8 位」
//   -> 预先算好 256 种情况 -> 运行时查表 + 异或高位
uint32_t Extend(uint32_t init_crc, const char* data, size_t n) {
    uint32_t crc = ~init_crc;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(data);
    for (size_t i = 0; i < n; i++) {
        crc = kTable.t[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    }
    return ~crc;
}

// ---------------- 参考版：逐位，逻辑最直白 ----------------
// 【只用于测试】：拿它和 Extend 对照，两者结果必须完全一致。
uint32_t ExtendBitwise(uint32_t init_crc, const char* data, size_t n) {
    uint32_t crc = ~init_crc;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(data);
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];                       // 把这一字节混进最低 8 位
        for (int j = 0; j < 8; j++) {      // 一位一位处理
            crc = (crc & 1u) ? ((crc >> 1) ^ kPoly) : (crc >> 1);
        }
    }
    return ~crc;
}

}  // namespace crc32c