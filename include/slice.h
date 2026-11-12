#ifndef LDB_SLICE_H
#define LDB_SLICE_H

#include <cstring>
#include <cstddef>
#include <string>

// Slice 是一个轻量的「字符串视图」：只持有指针 + 长度，不拥有内存。
// 为什么不用 std::string：
//   ① SSTable / WAL 里到处是「子串」，用 string 会疯狂拷贝
//   ② 我们只需要看，不需要改
class Slice {
public:
    Slice() : data_(""), size_(0) {}
    Slice(const char* d, size_t n) : data_(d), size_(n) {}
    // 从 C 字符串构造（strlen 算长度）
    Slice(const char* s) : data_(s), size_(strlen(s)) {}
    Slice(const std::string& s) : data_(s.data()), size_(s.size()) {}

    const char* data() const { return data_; }
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }

    char operator[](size_t n) const { return data_[n]; }

    std::string ToString() const { return std::string(data_, size_); }

    // 按字节比较（和 memcmp 语义一致）
    int Compare(const Slice& b) const;

    // 去掉前 n 个字节（【原地修改】，和 LevelDB 一致）
    void RemovePrefix(size_t n) { data_ += n; size_ -= n; }

private:
    const char* data_;
    size_t size_;
};

inline int Slice::Compare(const Slice& b) const {
    const size_t min_len = (size_ < b.size_) ? size_ : b.size_;
    int r = memcmp(data_, b.data_, min_len);
    if (r != 0) return r;
    if (size_ < b.size_) return -1;
    if (size_ > b.size_) return +1;
    return 0;
}


inline bool operator==(const Slice& a, const Slice& b) { return a.Compare(b) == 0; }

#endif  // LDB_SLICE_H