#include "block.h"

#include <cassert>
#include <cstring>

#include "util/coding.h"

namespace ldb {

// ================= BlockBuilder =================

BlockBuilder::BlockBuilder(int restart_interval) : restart_interval_(restart_interval) {
    restarts_.push_back(0);  // 第一个 entry 一定是 restart
}

void BlockBuilder::Reset() {
    buffer_.clear();
    restarts_.clear();
    restarts_.push_back(0);
    counter_ = 0;
    finished_entries_ = 0;
    last_key_.clear();
    finished_ = false;
}

void BlockBuilder::Add(const Slice& key, const Slice& value) {
    assert(!finished_);
    // key 必须严格递增（调用方保证：来自 MemTable 的有序遍历）
    assert(last_key_.empty() || key.Compare(Slice(last_key_)) > 0);

    size_t shared = 0;
    if (counter_ < restart_interval_) {
        // 计算和上一条的公共前缀长度
        const size_t min_len =
            (last_key_.size() < key.size()) ? last_key_.size() : key.size();
        while (shared < min_len && last_key_[shared] == key[shared]) {
            shared++;
        }
    } else {
        // 【restart 点】：不做压缩，重新开始
        restarts_.push_back(static_cast<uint32_t>(buffer_.size()));
        counter_ = 0;
    }

    const size_t non_shared = key.size() - shared;

    PutVarint32(&buffer_, static_cast<uint32_t>(shared));
    PutVarint32(&buffer_, static_cast<uint32_t>(non_shared));
    PutVarint32(&buffer_, static_cast<uint32_t>(value.size()));
    buffer_.append(key.data() + shared, non_shared);
    buffer_.append(value.data(), value.size());

    last_key_.assign(key.data(), key.size());
    counter_++;
    finished_entries_++;
}

Slice BlockBuilder::Finish() {
    // 写入所有 restart 偏移
    for (uint32_t offset : restarts_) {
        PutFixed32(&buffer_, offset);
    }
    // 写入 restart 个数
    PutFixed32(&buffer_, static_cast<uint32_t>(restarts_.size()));

    finished_ = true;
    return Slice(buffer_);
}

size_t BlockBuilder::CurrentSizeEstimate() const {
    // 当前 buffer + restart 数组 + count
    return buffer_.size() + restarts_.size() * sizeof(uint32_t) + sizeof(uint32_t);
}

// ================= Block =================

Block::Block(Slice contents) : contents_(contents) {
    assert(contents.size() >= sizeof(uint32_t));
    num_restarts_ = 0;
    // 最后 4 字节是 restart_count（小端）
    Slice tail(contents_.data() + contents_.size() - sizeof(uint32_t),
               sizeof(uint32_t));
    GetFixed32(&tail, &num_restarts_);

    // restart 数组在 count 之前
    restart_offset_ =
        static_cast<uint32_t>(contents_.size() - sizeof(uint32_t) -
                              num_restarts_ * sizeof(uint32_t));

    // 统计 entry 个数（只用于测试/调试）
    num_entries_ = 0;
    {
        Slice input = Slice(contents_.data(), restart_offset_);
        std::string last_key;
        while (input.size() > 0) {
            uint32_t shared = 0;
            Slice key_delta, value;
            if (!ParseEntry(&input, &shared, &key_delta, &value)) break;
            last_key.resize(shared);
            last_key.append(key_delta.data(), key_delta.size());
            num_entries_++;
        }
    }
}

uint32_t Block::RestartOffset(uint32_t i) const {
    uint32_t offset = 0;
    Slice input(contents_.data() + restart_offset_ + i * sizeof(uint32_t),
                sizeof(uint32_t));
    GetFixed32(&input, &offset);
    return offset;
}

bool Block::ParseEntry(Slice* input, uint32_t* shared, Slice* key_delta, Slice* value) {
    uint32_t non_shared = 0;
    uint32_t value_len = 0;
    if (!GetVarint32(input, shared)) return false;
    if (!GetVarint32(input, &non_shared)) return false;
    if (!GetVarint32(input, &value_len)) return false;

    if (input->size() < non_shared + value_len) return false;

    *key_delta = Slice(input->data(), non_shared);
    input->RemovePrefix(non_shared);

    *value = Slice(input->data(), value_len);
    input->RemovePrefix(value_len);
    return true;
}

int Block::CompareRestartKey(uint32_t i, const Slice& target) const {
    const uint32_t offset = RestartOffset(i);
    Slice input(contents_.data() + offset, restart_offset_ - offset);

    uint32_t shared = 0;
    Slice key_delta, value;
    if (!ParseEntry(&input, &shared, &key_delta, &value)) {
        return +1;  // 解析失败当作「大于」
    }
    // restart 点的 shared 一定是 0，key_delta 就是完整 key
    assert(shared == 0);
    return key_delta.Compare(target);
}

bool Block::SeekGE(const Slice& target, std::string* out_key, std::string* out_value) const {
    if (num_restarts_ == 0) return false;

    // ---------- ① 在 restart 数组里二分 ----------
    // 找「最后一个 restart 的 key <= target」
    // 如果所有 restart 的 key 都 > target，二分后 left 会是 0（要顺推）
    uint32_t left = 0;
    uint32_t right = num_restarts_ - 1;
    while (left < right) {
        const uint32_t mid = (left + right + 1) / 2;
        if (CompareRestartKey(mid, target) <= 0) {
            left = mid;
        } else {
            right = mid - 1;
        }
    }

    // ---------- ② 从那个 restart 开始顺推 ----------
    // 【注意】边界是 restart_offset_（数组区起点），不是 contents_.size()
    const uint32_t start = RestartOffset(left);
    Slice input(contents_.data() + start, restart_offset_ - start);

    std::string last_key;
    while (input.size() > 0) {
        uint32_t shared = 0;
        Slice key_delta, val;
        if (!ParseEntry(&input, &shared, &key_delta, &val)) break;

        // 还原完整 key（前 shared 字节来自上一条 + 本条独有部分）
        std::string cur_key;
        if (shared > 0) cur_key.assign(last_key.data(), shared);
        cur_key.append(key_delta.data(), key_delta.size());

        if (Slice(cur_key).Compare(target) >= 0) {
            *out_key = std::move(cur_key);
            out_value->assign(val.data(), val.size());
            return true;
        }
        last_key.swap(cur_key);
    }
    return false;
}

bool Block::Get(const Slice& key, std::string* value) const {
    std::string found_key;
    std::string found_value;
    if (!SeekGE(key, &found_key, &found_value)) return false;
    if (Slice(found_key).Compare(key) != 0) return false;
    *value = std::move(found_value);
    return true;
}

void Block::DumpTo(std::vector<std::pair<std::string, std::string>>* out) const {
    Slice input(contents_.data(), restart_offset_);
    std::string last_key;
    while (input.size() > 0) {
        uint32_t shared = 0;
        Slice key_delta, val;
        if (!ParseEntry(&input, &shared, &key_delta, &val)) break;

        std::string cur_key;
        if (shared > 0) cur_key.assign(last_key.data(), shared);
        cur_key.append(key_delta.data(), key_delta.size());

        out->emplace_back(cur_key, std::string(val.data(), val.size()));
        last_key.swap(cur_key);
    }
}

}  // namespace ldb