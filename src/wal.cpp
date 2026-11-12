#include "wal.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "util/coding.h"
#include "util/crc32c.h"

namespace wal {

namespace {

// 把 errno 转成可读的错误信息
std::string ErrnoMsg(const char* what) {
    return std::string(what) + ": " + std::strerror(errno);
}

}  // namespace

// ================= Writer =================

Writer::~Writer() { Close(); }

Status Writer::Open(const std::string& path) {
    path_ = path;
    // O_APPEND：总是追加到末尾（WAL 只追加，不改写）
    fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd_ < 0) return Status::IOError(ErrnoMsg(("open " + path).c_str()));
    block_.reserve(kBlockSize);
    return Status::OK();
}

// 把 block_ 里的内容真正 write() 到文件
Status Writer::FlushBlock() {
    if (block_.empty()) return Status::OK();

    size_t written = 0;
    while (written < block_.size()) {
        ssize_t r = ::write(fd_, block_.data() + written, block_.size() - written);
        if (r < 0) {
            if (errno == EINTR) continue;  // 被信号打断，重试
            return Status::IOError(ErrnoMsg("write"));
        }
        written += static_cast<size_t>(r);
    }
    bytes_written_ += block_.size();
    block_.clear();
    dirty_ = true;
    return Status::OK();
}

Status Writer::AddRecord(const Slice& data) {
    const size_t need = kHeaderSize + data.size();
    if (need > kBlockSize) {
        return Status::InvalidArgument("record larger than one block");
    }

    // 规则①：当前 block 放不下 → 填充到末尾，然后开新 block
    if (block_.size() + need > kBlockSize) {
        block_.append(kBlockSize - block_.size(), '\0');  // 填 0
        Status s = FlushBlock();
        if (!s.ok()) return s;
    }

    // 组一条记录
    std::string rec;
    rec.reserve(need);
    PutFixed32(&rec, 0);                                   // crc 占位
    PutFixed32(&rec, static_cast<uint32_t>(data.size()));  // length
    rec.append(data.data(), data.size());

    // crc 覆盖 [length + data]（跳过 crc 自己那 4 字节）
    const uint32_t crc = crc32c::Value(rec.data() + 4, rec.size() - 4);
    rec[0] = static_cast<char>(crc & 0xff);
    rec[1] = static_cast<char>((crc >> 8) & 0xff);
    rec[2] = static_cast<char>((crc >> 16) & 0xff);
    rec[3] = static_cast<char>((crc >> 24) & 0xff);

    block_.append(rec);
    return Status::OK();
}

Status Writer::Sync() {
    Status s = FlushBlock();
    if (!s.ok()) return s;
    if (dirty_ && fd_ >= 0) {
        // fsync：把 OS 页缓存强制刷到磁盘
        // 没有它，断电时数据还在 OS 缓存里 -> 丢失
        if (::fsync(fd_) != 0) return Status::IOError(ErrnoMsg("fsync"));
        dirty_ = false;
    }
    return Status::OK();
}

Status Writer::Close() {
    if (fd_ < 0) return Status::OK();
    Status s = Sync();
    ::close(fd_);
    fd_ = -1;
    return s;
}

// ================= Reader =================

Reader::~Reader() { Close(); }

Status Reader::Open(const std::string& path) {
    path_ = path;
    fd_ = ::open(path.c_str(), O_RDONLY);
    if (fd_ < 0) {
        if (errno == ENOENT) return Status::NotFound("wal not found: " + path);
        return Status::IOError(ErrnoMsg(("open " + path).c_str()));
    }
    block_.clear();
    block_pos_ = 0;
    return Status::OK();
}

bool Reader::LoadNextBlock() {
    block_.assign(kBlockSize, '\0');
    size_t got = 0;
    while (got < kBlockSize) {
        ssize_t r = ::read(fd_, &block_[got], kBlockSize - got);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) break;  // EOF
        got += static_cast<size_t>(r);
    }
    if (got == 0) {
        block_.clear();
        return false;
    }
    block_.resize(got);  // 最后一块可能不满
    block_pos_ = 0;
    return true;
}

Status Reader::ReadRecord(std::string* record) {
    while (true) {
        // 当前 block 剩余不足一个头部 → 读下一个 block
        if (block_pos_ + kHeaderSize > block_.size()) {
            if (!LoadNextBlock()) return Status::NotFound("end of wal");
            continue;
        }

        const char* p = block_.data() + block_pos_;

        // 规则③：填充区（crc=0 且 length=0）→ 跳到下一个 block
        uint32_t crc = 0, len = 0;
        {
            Slice s(p, block_.size() - block_pos_);
            if (!GetFixed32(&s, &crc) || !GetFixed32(&s, &len)) {
                if (!LoadNextBlock()) return Status::NotFound("end of wal");
                continue;
            }
        }
        if (crc == 0 && len == 0) {
            if (!LoadNextBlock()) return Status::NotFound("end of wal");
            continue;
        }

        // 规则②：长度越界 → 数据损坏（末尾被截断）
        if (block_pos_ + kHeaderSize + len > block_.size()) {
            return Status::Corruption("truncated record at offset");
        }

        // CRC 校验：重算一遍对比
        const uint32_t actual = crc32c::Value(p + 4, 4 + len);
        if (actual != crc) {
            return Status::Corruption("crc mismatch");
        }

        record->assign(p + kHeaderSize, len);
        block_pos_ += kHeaderSize + len;
        return Status::OK();
    }
}

Status Reader::Close() {
    if (fd_ < 0) return Status::OK();
    ::close(fd_);
    fd_ = -1;
    return Status::OK();
}

}  // namespace wal