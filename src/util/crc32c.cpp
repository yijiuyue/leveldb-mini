#include "util/crc32c.h"

namespace crc32c {

namespace {

// CRC32C 的多项式（Castagnoli），反射形式
constexpr uint32_t kPoly = 0x82f63b78u;

// 编译期生成 256 项查找表（不用手写，也不占运行时间）
struct Table {
    uint32_t t[256];
    constexpr Table() : t() {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t crc = i;
            for (int j = 0; j < 8; j++) {
                crc = (crc & 1u) ? ((crc >> 1) ^ kPoly) : (crc >> 1);
            }
            t[i] = crc;
        }
    }
};

constexpr Table kTable{};

}  // namespace

uint32_t Extend(uint32_t init_crc, const char* data, size_t n) {
    uint32_t crc = ~init_crc;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(data);
    for (size_t i = 0; i < n; i++) {
        crc = kTable.t[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    }
    return ~crc;
}


}  // namespace crc32c