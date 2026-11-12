#ifndef LDB_UTIL_CODING_H
#define LDB_UTIL_CODING_H

#include <cstdint>
#include <string>

#include "slice.h"

// ============ varint ============
// 变长整数：小数字占少字节。
//   0 .. 127        -> 1 字节
//   128 .. 16383    -> 2 字节
//   ...
// 编码方式：每字节低 7 位放数据，最高位是「还有后续」标志。
//
// 为什么需要：SSTable / WAL 里到处是长度字段，绝大多数 < 128，
// 用 varint 平均每个字段省 2-3 字节。

void PutVarint32(std::string* dst, uint32_t value);
void PutVarint64(std::string* dst, uint64_t value);

// 从 input 里解出一个 varint，成功返回 true 并把 *input 前移。
// 【必须带长度检查】——数据损坏时不能越界读。
bool GetVarint32(Slice* input, uint32_t* value);
bool GetVarint64(Slice* input, uint64_t* value);

// 不带长度检查的版本（仅用于已知安全的位置，比如自己刚写的数据）
const char* GetVarint32Ptr(const char* p, const char* limit, uint32_t* value);

// ============ 定长编码 ============
// 小端序（x86/ARM 原生），跨平台需要转字节序。
void PutFixed32(std::string* dst, uint32_t value);
void PutFixed64(std::string* dst, uint64_t value);

bool GetFixed32(Slice* input, uint32_t* value);
bool GetFixed64(Slice* input, uint64_t* value);

// ============ 长度前缀 ============
// 写「长度 + 内容」，读时先读长度再取内容。
void PutLengthPrefixedSlice(std::string* dst, const Slice& value);
bool GetLengthPrefixedSlice(Slice* input, Slice* result);

#endif  // LDB_UTIL_CODING_H