#include "memtable.h"

namespace ldb {

namespace {

int StringCompare(const std::string& a, const std::string& b) {
    if (a < b) return -1;
    if (a > b) return +1;
    return 0;
}

// 把 (type, value) 编码成跳表里存的 value
std::string EncodeValue(ValueType type, const std::string& value) {
    std::string out;
    out.reserve(1 + value.size());
    out.push_back(static_cast<char>(type));
    if (type == kTypeValue) out.append(value);
    return out;
}

}  // namespace

MemTable::MemTable() : table_(StringCompare) {}
MemTable::~MemTable() = default;

void MemTable::Put(const std::string& key, const std::string& value) {
    table_.Insert(key, EncodeValue(kTypeValue, value));
}

void MemTable::Delete(const std::string& key) {
    table_.Insert(key, EncodeValue(kTypeDeletion, std::string()));
}

MemTableResult::Kind MemTable::Get(const std::string& key, std::string* value) const {
    std::string encoded;
    if (!table_.Get(key, &encoded)) return MemTableResult::kNotFound;

    const unsigned char type = static_cast<unsigned char>(encoded[0]);
    if (type == kTypeDeletion) {
        value->clear();
        return MemTableResult::kDeleted;
    }
    value->assign(encoded.data() + 1, encoded.size() - 1);
    return MemTableResult::kFound;
}

}  // namespace ldb