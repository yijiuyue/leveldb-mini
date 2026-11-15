#ifndef LDB_DB_H
#define LDB_DB_H

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "memtable.h"
#include "slice.h"
#include "status.h"
#include "wal.h"

// ============ DB：把三层串起来 ============
//
//   Put(k, v):
//     ① 追加到 WAL（顺序写，快）
//     ② 写进 MemTable（内存跳表，有序）
//     ③ MemTable 满了 → 落成 SSTable
//
//   Get(k):
//     ① 查 MemTable
//     ② 没找到 → 按【新的优先】查 SSTable 列表
//
//   Open(path):
//     ① 扫描目录，收集所有 .sst
//     ② 重放 .log 到 MemTable（崩溃恢复）
//
// ============ 目录布局 ============
//
//   mydb/
//   ├── 000001.log      WAL（当前写入的）
//   ├── 000002.sst      SSTable（按编号，越大越新）
//   └── 000003.sst
//
// ============ 已知限制（L1.5） ============
//   ① 没有 compaction：SSTable 只增不减 → 读放大随文件数线性增长
//   ② 每次 Get 都 open/close 一个 SSTable（真实 LevelDB 用 TableCache）
//   ③ 读也加了锁（真实实现读是无锁的）
//   ④ 没有布隆过滤器（下一步）
//   ⑤ 没有 MANIFEST（用文件列表代替）

namespace ldb {

struct Options {
    // MemTable 攒到多大就落盘（真实 LevelDB 默认 4MB）
    size_t write_buffer_size = 4 * 1024 * 1024;
    size_t block_size = 4096;
};

class DB {
public:
    // 打开（不存在就创建）。*db 拿到所有权
    static Status Open(const std::string& dir, const Options& opt,
                       std::unique_ptr<DB>* db);

    ~DB();

    DB(const DB&) = delete;
    DB& operator=(const DB&) = delete;

    // 写
    //   sync = true  -> 返回 OK 时保证已 fsync 落盘
    //   sync = false -> 返回 OK 时可能还在 OS 页缓存
    Status Put(const Slice& key, const Slice& value, bool sync = false);
    Status Delete(const Slice& key, bool sync = false);

    // 读：OK / NotFound
    Status Get(const Slice& key, std::string* value);

    // 强制把 MemTable 落成 SSTable（测试和 Close 用）
    Status Flush();

    size_t NumSSTables() const { return sst_files_.size(); }
    size_t MemTableSize() const { return mem_->ApproximateMemoryUsage(); }

private:
    DB() = default;

    Status Recover();       // 扫目录 + 重放 WAL
    Status FlushMemTable(); // 把 MemTable 写成 SSTable，换新 WAL

    std::string dir_;
    Options opt_;

    // 单写者模型：写操作串行；读也复用这把锁（简化）
    std::mutex mu_;

    std::unique_ptr<wal::Writer> wal_;
    std::string wal_path_;              // 当前 WAL 的路径（flush 后要删）
    std::unique_ptr<MemTable> mem_;

    // 按文件名升序 → 后面的更新
    std::vector<std::string> sst_files_;

    uint64_t next_file_number_ = 1;
};

}  // namespace ldb

#endif  // LDB_DB_H