#include "util/coding.h"

#include <cstring>

// ---------------- varint 编码 ----------------

void PutVarint64(std::string* dst, uint64_t value) {
    // 每轮取低 7 位；如果还有剩余，最高位置 1
    while (value >= 0x80) {
        dst->push_back(static_cast<char>((value & 0x7f) | 0x80));
        value >>= 7;
    }
    dst->push_back(static_cast<char>(value));
}

void PutVarint32(std::string* dst, uint32_t value) {
    PutVarint64(dst, value);
}

// ---------------- varint 解码 ----------------

const char* GetVarint64Ptr(const char* p, const char* limit, uint64_t* value) {
    uint64_t result = 0;
    for (uint32_t shift = 0; shift <= 63 && p < limit; shift += 7) {
        const uint64_t byte = static_cast<unsigned char>(*p);
        p++;
        if (byte & 0x80) {
            // 还有后续字节
            result |= ((byte & 0x7f) << shift);
        } else {
            result |= (byte << shift);
            *value = result;
            return p;
        }
    }
    return nullptr;  // 越界 / 数据损坏
}

const char* GetVarint32Ptr(const char* p, const char* limit, uint32_t* value) {
    uint64_t v = 0;
    const char* end = GetVarint64Ptr(p, limit, &v);
    if (end == nullptr) return nullptr;
    if (v > 0xffffffffULL) return nullptr;  // 溢出
    *value = static_cast<uint32_t>(v);
    return end;
}

bool GetVarint64(Slice* input, uint64_t* value) {
    const char* p = GetVarint64Ptr(input->data(), input->data() + input->size(), value);
    if (p == nullptr) return false;
    input->RemovePrefix(static_cast<size_t>(p - input->data()));
    return true;
}

bool GetVarint32(Slice* input, uint32_t* value) {
    const char* p = GetVarint32Ptr(input->data(), input->data() + input->size(), value);
    if (p == nullptr) return false;
    input->RemovePrefix(static_cast<size_t>(p - input->data()));
    return true;
}

// ---------------- 定长编码（小端） ----------------

void PutFixed32(std::string* dst, uint32_t value) {
    char buf[4];
    buf[0] = static_cast<char>(value & 0xff);
    buf[1] = static_cast<char>((value >> 8) & 0xff);
    buf[2] = static_cast<char>((value >> 16) & 0xff);
    buf[3] = static_cast<char>((value >> 24) & 0xff);
    dst->append(buf, 4);
}

void PutFixed64(std::string* dst, uint64_t value) {
    char buf[8];
    for (int i = 0; i < 8; i++) {
        buf[i] = static_cast<char>((value >> (8 * i)) & 0xff);
    }
    dst->append(buf, 8);
}

bool GetFixed32(Slice* input, uint32_t* value) {
    if (input->size() < 4) return false;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(input->data());
    *value = static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
             (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
    input->RemovePrefix(4);
    return true;
}

bool GetFixed64(Slice* input, uint64_t* value) {
    if (input->size() < 8) return false;
    const unsigned char* p = reinterpret_cast<const unsigned char*>(input->data());
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= (static_cast<uint64_t>(p[i]) << (8 * i));
    }
    *value = v;
    input->RemovePrefix(8);
    return true;
}

// ---------------- 长度前缀 ----------------

void PutLengthPrefixedSlice(std::string* dst, const Slice& value) {
    PutVarint32(dst, static_cast<uint32_t>(value.size()));
    dst->append(value.data(), value.size());
}

bool GetLengthPrefixedSlice(Slice* input, Slice* result) {
    uint32_t len = 0;
    if (!GetVarint32(input, &len)) return false;
    if (input->size() < len) return false;
    *result = Slice(input->data(), len);
    input->RemovePrefix(len);
    return true;
}