#ifndef LDB_UTIL_CRC32C_H
#define LDB_UTIL_CRC32C_H

#include <cstddef>
#include <cstdint>

// CRC32C（Castagnoli）：用来检测数据损坏。
//
// 为什么 WAL 每条记录都要 CRC：
//   写到一半断电会留下「半条记录」。
//   没有 CRC -> 读的时候把半条当完整的 -> 数据错乱。
//   有 CRC   -> 校验失败 -> 知道这条不完整 -> 停在这里。
//
// 实现：查表法（256 项），一次处理 1 字节。
namespace crc32c {

// data 的 CRC，extend 支持「接着上一段算」（WAL 分片写时有用）
uint32_t Extend(uint32_t init_crc, const char* data, size_t n);

inline uint32_t Value(const char* data, size_t n) { return Extend(0, data, n); }

}  // namespace crc32c

#endif  // LDB_UTIL_CRC32C_H