#include "db.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <system_error>

#include "table.h"
#include "util/coding.h"

namespace fs = std::filesystem;

namespace ldb {

namespace {

// WAL 里一条记录的格式：
//   type(1) | key_len(varint) | key | value_len(varint) | value
enum RecordType : unsigned char {
    kRecPut = 1,
    kRecDelete = 2,
};

std::string EncodeRecord(RecordType type, const Slice& key, const Slice& value) {
    std::string out;
    out.push_back(static_cast<char>(type));
    PutLengthPrefixedSlice(&out, key);
    PutLengthPrefixedSlice(&out, value);
    return out;
}

bool DecodeRecord(const Slice& data, RecordType* type, std::string* key,
                  std::string* value) {
    if (data.size() < 1) return false;
    *type = static_cast<RecordType>(static_cast<unsigned char>(data[0]));

    Slice input = data;
    input.RemovePrefix(1);
    Slice k, v;
    if (!GetLengthPrefixedSlice(&input, &k)) return false;
    if (!GetLengthPrefixedSlice(&input, &v)) return false;

    key->assign(k.data(), k.size());
    value->assign(v.data(), v.size());
    return true;
}

// SSTable 里存的 value = 和 MemTable 一致：type(1) + payload
std::string EncodeSSTValue(ValueType type, const std::string& payload) {
    std::string out;
    out.push_back(static_cast<char>(type));
    if (type == kTypeValue) out.append(payload);
    return out;
}

std::string FileName(const std::string& dir, uint64_t number, const char* ext) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%06llu", (unsigned long long)number);
    return dir + "/" + buf + ext;
}

}  // namespace

// ================= Open / Recover =================

Status DB::Open(const std::string& dir, const Options& opt, std::unique_ptr<DB>* db) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
        return Status::IOError("create_directories " + dir + ": " + ec.message());
    }

    std::unique_ptr<DB> d(new DB());
    d->dir_ = dir;
    d->opt_ = opt;

    Status s = d->Recover();
    if (!s.ok()) return s;

    *db = std::move(d);
    return Status::OK();
}

Status DB::Recover() {
    // ---------- ① 扫描目录 ----------
    std::vector<std::string> log_files;
    std::uint64_t max_number = 0;

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(dir_, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        const std::string path = entry.path().string();
        const std::string ext = entry.path().extension().string();

        // 从文件名解析编号（"000003" -> 3）
        const std::string stem = entry.path().stem().string();
        std::uint64_t num = 0;
        try {
            num = std::stoull(stem);
        } catch (...) {
            continue;  // 不是我的文件，跳过
        }

        if (ext == ".sst") {
            sst_files_.push_back(path);
        } else if (ext == ".log") {
            log_files.push_back(path);
        } else {
            continue;
        }
        if (num > max_number) max_number = num;
    }
    std::sort(sst_files_.begin(), sst_files_.end());
    std::sort(log_files.begin(), log_files.end());

    // ---------- ② 重放 WAL（崩溃恢复） ----------
    mem_.reset(new MemTable());
    int recovered = 0;

    if (!log_files.empty()) {
        // 用编号最大的那个 log（正常情况只有一个）
        const std::string& wal_path = log_files.back();
        wal::Reader reader;
        Status s = reader.Open(wal_path);
        if (!s.ok() && !s.IsNotFound()) return s;

        std::string rec;
        while (true) {
            s = reader.ReadRecord(&rec);
            if (s.IsNotFound()) break;          // 正常读完
            if (s.IsCorruption()) {
                // 【静默截断】崩溃时最后一条可能只写了一半
                // 前面恢复出来的都是好的，后面的都不可信 → 停
                break;
            }
            if (!s.ok()) return s;

            RecordType type;
            std::string key, value;
            if (!DecodeRecord(Slice(rec), &type, &key, &value)) continue;

            if (type == kRecPut) {
                mem_->Put(key, value);
            } else if (type == kRecDelete) {
                mem_->Delete(key);
            }
            recovered++;
        }
        reader.Close();
    }

    // ---------- ③ 删掉陈旧的 WAL（数据已在 SSTable 里） ----------
    // 保留编号最大的那个（上面已重放），其余全删
    for (size_t i = 0; i + 1 < log_files.size(); i++) {
        std::error_code ec2;
        fs::remove(log_files[i], ec2);
    }

    // ---------- ④ 打开新 WAL（编号 = max + 1） ----------
    next_file_number_ = max_number + 1;
    wal_path_ = FileName(dir_, next_file_number_, ".log");
    next_file_number_++;

    wal_.reset(new wal::Writer());
    Status s = wal_->Open(wal_path_);
    if (!s.ok()) return s;

    // 恢复出来的数据如果已经超过阈值，落盘
    if (mem_->ApproximateMemoryUsage() >= opt_.write_buffer_size) {
        return FlushMemTable();
    }
    (void)recovered;
    return Status::OK();
}

// ================= 写 =================

Status DB::Put(const Slice& key, const Slice& value, bool sync) {
    std::lock_guard<std::mutex> lock(mu_);

    // ① 【先写 WAL】—— 顺序不能反，反了崩溃就丢数据
    const std::string rec = EncodeRecord(kRecPut, key, value);
    Status s = wal_->AddRecord(Slice(rec));
    if (!s.ok()) return s;
    if (sync) {
        s = wal_->Sync();
        if (!s.ok()) return s;
    }

    // ② 再写 MemTable
    mem_->Put(key.ToString(), value.ToString());

    // ③ 检查是否需要落盘
    if (mem_->ApproximateMemoryUsage() >= opt_.write_buffer_size) {
        return FlushMemTable();
    }
    return Status::OK();
}

Status DB::Delete(const Slice& key, bool sync) {
    std::lock_guard<std::mutex> lock(mu_);

    const std::string rec = EncodeRecord(kRecDelete, key, Slice());
    Status s = wal_->AddRecord(Slice(rec));
    if (!s.ok()) return s;
    if (sync) {
        s = wal_->Sync();
        if (!s.ok()) return s;
    }

    // 写一条墓碑（不是真删）
    mem_->Delete(key.ToString());

    if (mem_->ApproximateMemoryUsage() >= opt_.write_buffer_size) {
        return FlushMemTable();
    }
    return Status::OK();
}

// ================= 读 =================

Status DB::Get(const Slice& key, std::string* value) {
    std::lock_guard<std::mutex> lock(mu_);

    // ① 先查 MemTable（最新）
    std::string v;
    const auto kind = mem_->Get(key.ToString(), &v);
    if (kind == MemTableResult::kFound) {
        *value = std::move(v);
        return Status::OK();
    }
    if (kind == MemTableResult::kDeleted) {
        return Status::NotFound("deleted in memtable");
    }

    // ② 按【新的优先】查 SSTable
    for (auto it = sst_files_.rbegin(); it != sst_files_.rend(); ++it) {
        std::unique_ptr<Table> table;
        Status s = Table::Open(*it, &table);
        if (!s.ok()) continue;  // 坏文件跳过（真实实现应该报错）

        std::string encoded;
        s = table->Get(key, &encoded);
        if (s.IsNotFound()) continue;  // 这个文件没有，查更老的
        if (!s.ok()) continue;

        // 解码 type
        const unsigned char type = static_cast<unsigned char>(encoded[0]);
        if (type == kTypeDeletion) {
            // 墓碑：明确不存在，【不再往更老的文件找】
            return Status::NotFound("deleted in sstable");
        }
        value->assign(encoded.data() + 1, encoded.size() - 1);
        return Status::OK();
    }

    return Status::NotFound("key not found");
}

// ================= 落盘 =================

Status DB::Flush() {
    std::lock_guard<std::mutex> lock(mu_);
    return FlushMemTable();
}

Status DB::FlushMemTable() {
    if (mem_->size() == 0) return Status::OK();

    // ① 写新 SSTable
    const std::string sst_path = FileName(dir_, next_file_number_, ".sst");
    next_file_number_++;

    TableBuilder builder(sst_path, opt_.block_size);
    Status s;
    mem_->ForEach([&](const std::string& key, ValueType type, const std::string& payload) {
        if (!s.ok()) return;
        const std::string encoded = EncodeSSTValue(type, payload);
        s = builder.Add(Slice(key), Slice(encoded));
    });
    if (!s.ok()) return s;

    s = builder.Finish();
    if (!s.ok()) return s;

    sst_files_.push_back(sst_path);
    std::sort(sst_files_.begin(), sst_files_.end());

    // ② 清空 MemTable
    mem_.reset(new MemTable());

    // ③ 换新 WAL（旧的可以删了：数据已经在 SSTable 里）
    if (wal_) {
        wal_->Close();
        wal_.reset();
        if (!wal_path_.empty()) {
            std::error_code ec3;
            fs::remove(wal_path_, ec3);   // 删掉旧 WAL
        }
    }
    wal_path_ = FileName(dir_, next_file_number_, ".log");
    next_file_number_++;
    wal_.reset(new wal::Writer());
    return wal_->Open(wal_path_);
}

DB::~DB() {
    if (wal_) {
        // 关闭前 fsync：保证最后一批写入落盘
        wal_->Close();
    }
}

}  // namespace ldb