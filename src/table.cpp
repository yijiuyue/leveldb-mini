#include "table.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>

#include "util/coding.h"

namespace ldb {

namespace {

// magic：校验「这个文件确实是我的 SSTable」
const char kMagic[8] = {'l', 'e', 'v', 'e', 'l', 'd', 'b', ':'};
constexpr size_t kFooterSize = 40;  // 4 × 8 偏移 + 8 magic

std::string ErrnoMsg(const char* what) {
    return std::string(what) + ": " + std::strerror(errno);
}

// 取路径的目录部分（"a/b/c.sst" -> "a/b"，"c.sst" -> "."）
std::string DirOf(const std::string& path) {
    const size_t pos = path.find_last_of('/');
    if (pos == std::string::npos) return ".";
    if (pos == 0) return "/";
    return path.substr(0, pos);
}

// fsync 一个目录，让「新建文件的目录项」落盘
//
// 【为什么需要】新文件的 dentry 和 inode 都在内核缓存里，
// 只 fsync 文件内容，断电后文件名可能不存在（ENOENT）。
bool SyncDir(const std::string& dir) {
    const int dirfd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (dirfd < 0) return false;
    const int rc = ::fsync(dirfd);
    const int saved = errno;
    ::close(dirfd);
    errno = saved;
    return rc == 0;
}

}  // namespace

// ================= TableBuilder =================

TableBuilder::TableBuilder(const std::string& path, size_t block_size)
    : path_(path), block_size_(block_size) {
    fd_ = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd_ < 0) {
        status_ = Status::IOError(ErrnoMsg(("open " + path).c_str()));
    }
}

TableBuilder::~TableBuilder() {
    if (fd_ >= 0) {
        if (!finished_) {
            // 没调用 Finish 就析构：数据不完整，丢弃（不 fsync）
            // 调用方应该检查 Finish 的返回值
        }
        ::close(fd_);
        fd_ = -1;
    }
}

Status TableBuilder::WriteRaw(const char* data, size_t n) {
    size_t written = 0;
    while (written < n) {
        ssize_t r = ::write(fd_, data + written, n - written);
        if (r < 0) {
            if (errno == EINTR) continue;
            return Status::IOError(ErrnoMsg("write"));
        }
        written += static_cast<size_t>(r);
    }
    offset_ += n;
    return Status::OK();
}

Status TableBuilder::Add(const Slice& key, const Slice& value) {
    if (!status_.ok()) return status_;
    if (finished_) return Status::InvalidArgument("Add after Finish");

    // key 必须严格递增
    if (has_last_key_ && key.Compare(Slice(last_key_)) <= 0) {
        return Status::InvalidArgument("keys must be added in increasing order");
    }
    last_key_.assign(key.data(), key.size());
    has_last_key_ = true;

    data_block_.Add(key, value);
    pending_index_key_ = last_key_;
    num_entries_++;

    // 收集哈希，Finish 时生成布隆位图
    key_hashes_.push_back(BloomHash(key));

    // block 攒到 4KB 就落盘
    if (data_block_.CurrentSizeEstimate() >= block_size_) {
        return Flush();
    }
    return Status::OK();
}

Status TableBuilder::Flush() {
    if (data_block_.empty()) return Status::OK();

    const uint64_t block_offset = offset_;
    Slice contents = data_block_.Finish();
    const uint32_t block_size = static_cast<uint32_t>(contents.size());

    Status s = WriteRaw(contents.data(), contents.size());
    if (!s.ok()) return s;

    // index 加一条：key = 本 block 的最后一个 key
    //               value = (block_offset, block_size)
    std::string handle;
    PutVarint32(&handle, static_cast<uint32_t>(block_offset));
    PutVarint32(&handle, block_size);
    index_block_.Add(Slice(pending_index_key_), Slice(handle));

    data_block_.Reset();
    num_data_blocks_++;
    return Status::OK();
}

Status TableBuilder::Finish() {
    if (!status_.ok()) return status_;
    if (finished_) return Status::OK();

    // ① 最后一批 data block
    Status s = Flush();
    if (!s.ok()) return s;

    // ② index block
    Slice index_contents = index_block_.Finish();
    const uint64_t index_offset = offset_;
    const uint64_t index_size = index_contents.size();
    s = WriteRaw(index_contents.data(), index_contents.size());
    if (!s.ok()) return s;

    // ③ 布隆过滤器（整文件一个）
    const std::string filter = bloom_policy_.CreateFilter(key_hashes_);
    const uint64_t filter_offset = offset_;
    const uint64_t filter_size = filter.size();
    s = WriteRaw(filter.data(), filter.size());
    if (!s.ok()) return s;

    // ④ footer（40 字节）
    std::string footer;
    PutFixed64(&footer, filter_offset);
    PutFixed64(&footer, filter_size);
    PutFixed64(&footer, index_offset);
    PutFixed64(&footer, index_size);
    footer.append(kMagic, sizeof(kMagic));
    // putFixed64 × 4 = 32 字节，+ 8 magic = 40 ✅

    s = WriteRaw(footer.data(), footer.size());
    if (!s.ok()) return s;

    // ⑤ fsync 文件内容
    //
    // 【只有 fsync 返回，数据才算真正落盘】
    //   write()  → 进 OS 页缓存
    //   close()  → 只是释放 fd，不保证刷盘
    //   fsync()  → 才保证落到物理磁盘
    if (::fsync(fd_) != 0) {
        return Status::IOError(ErrnoMsg("fsync"));
    }

    // ⑥ fsync 目录项
    //
    // 【为什么文件内容 fsync 了还不够】
    //   open(path, O_CREAT) 会在内核里创建【目录项（dentry）】和 inode，
    //   目录项本身也是页缓存里的数据。
    //   只 fsync 文件 fd，保证的是「文件内容」落盘，
    //   但「这个文件名存在」这件事可能还在缓存里。
    //   → 断电重启后 open("xxx.sst") 可能返回 ENOENT，文件「消失」。
    //
    //   所以对【新建的文件】必须再 fsync 它所在的目录。
    if (!SyncDir(DirOf(path_))) {
        return Status::IOError(ErrnoMsg("fsync dir"));
    }

    finished_ = true;
    return Status::OK();
}

// ================= Table =================

Table::Table() = default;

Table::~Table() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

Status Table::ReadRange(uint64_t offset, uint64_t size, std::string* out) const {
    out->assign(size, '\0');
    size_t got = 0;
    while (got < size) {
        ssize_t r = ::pread(fd_, &(*out)[got], size - got, static_cast<off_t>(offset + got));
        if (r < 0) {
            if (errno == EINTR) continue;
            return Status::IOError(ErrnoMsg("pread"));
        }
        if (r == 0) break;  // 文件比预期短
        got += static_cast<size_t>(r);
    }
    if (got != size) {
        return Status::Corruption("short read");
    }
    return Status::OK();
}

Status Table::ReadBlock(uint64_t offset, uint64_t size, std::string* out) const {
    return ReadRange(offset, size, out);
}

Status Table::Open(const std::string& path, std::unique_ptr<Table>* table) {
    std::unique_ptr<Table> t(new Table());
    t->path_ = path;
    t->fd_ = ::open(path.c_str(), O_RDONLY);
    if (t->fd_ < 0) {
        if (errno == ENOENT) return Status::NotFound("table not found: " + path);
        return Status::IOError(ErrnoMsg(("open " + path).c_str()));
    }

    // 文件总大小
    off_t sz = ::lseek(t->fd_, 0, SEEK_END);
    if (sz < 0) return Status::IOError(ErrnoMsg("lseek"));
    t->file_size_ = static_cast<uint64_t>(sz);

    if (t->file_size_ < kFooterSize) {
        return Status::Corruption("file too small to be a table");
    }

    // ① 读 footer（最后 40 字节）
    std::string footer;
    Status s = t->ReadRange(t->file_size_ - kFooterSize, kFooterSize, &footer);
    if (!s.ok()) return s;

    Slice f(footer);
    uint64_t filter_offset = 0, filter_size = 0, index_offset = 0, index_size = 0;
    if (!GetFixed64(&f, &filter_offset) || !GetFixed64(&f, &filter_size) ||
        !GetFixed64(&f, &index_offset) || !GetFixed64(&f, &index_size)) {
        return Status::Corruption("bad footer");
    }


    // ② 校验 magic
    if (f.size() < sizeof(kMagic) || std::memcmp(f.data(), kMagic, sizeof(kMagic)) != 0) {
        return Status::Corruption("bad magic");
    }

    // ③ 读布隆过滤器（filter_size == 0 表示没有，兼容旧文件）
    if (filter_size > 0) {
        if (filter_offset + filter_size > t->file_size_ - kFooterSize) {
            return Status::Corruption("filter out of range");
        }
        s = t->ReadRange(filter_offset, filter_size, &t->filter_data_);
        if (!s.ok()) return s;
    }

    // ④ 校验 index 的位置合法
    if (index_offset + index_size > t->file_size_ - kFooterSize) {
        return Status::Corruption("index out of range");
    }

    // ⑤ 把 index 读进内存（它很小，约数据量的 0.5%）
    s = t->ReadRange(index_offset, index_size, &t->index_data_);
    if (!s.ok()) return s;

    t->index_.reset(new Block(Slice(t->index_data_)));

    *table = std::move(t);
    return Status::OK();
}

Status Table::Get(const Slice& key, std::string* value) const {
    // ⓪ 布隆过滤器：如果【一定不在】，直接返回，省掉 pread
    //
    // 【为什么这个判断可信】
    //   布隆过滤器没有假阴性 —— 存在的 key 一定返回 true
    //   所以返回 false 时「一定不在」是确定的
    if (!filter_data_.empty() && !bloom_policy_.MayContain(Slice(filter_data_), key)) {
        return Status::NotFound("bloom filter: definitely not in this file");
    }

    // ① 在 index 里找「第一个 key >= target」的条目
    std::string index_key;
    std::string handle;
    if (!index_->SeekGE(key, &index_key, &handle)) {
        return Status::NotFound("key out of range");
    }

    // ② 解析 handle: (block_offset, block_size)
    Slice h(handle);
    uint32_t block_offset = 0, block_size = 0;
    if (!GetVarint32(&h, &block_offset) || !GetVarint32(&h, &block_size)) {
        return Status::Corruption("bad index handle");
    }

    // ③ 读那个 data block，在 block 内查找
    std::string block_data;
    Status s = ReadBlock(block_offset, block_size, &block_data);
    if (!s.ok()) return s;

    Block block{Slice(block_data)};
    if (!block.Get(key, value)) {
        return Status::NotFound("key not in block");
    }
    return Status::OK();
}

}  // namespace ldb