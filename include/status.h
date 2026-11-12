#ifndef LDB_STATUS_H
#define LDB_STATUS_H

#include <string>

// Status 表示一次操作的成败。
// 为什么不用异常：存储引擎的错误（IO 失败、数据损坏）是可预期的，
// 而且析构函数里不能抛异常 —— 用返回值更可控。
class Status {
public:
    Status() : code_(kOk) {}

    static Status OK() { return Status(); }
    static Status NotFound(const std::string& msg) { return Status(kNotFound, msg); }
    static Status Corruption(const std::string& msg) { return Status(kCorruption, msg); }
    static Status IOError(const std::string& msg) { return Status(kIOError, msg); }
    static Status InvalidArgument(const std::string& msg) {
        return Status(kInvalidArgument, msg);
    }

    bool ok() const { return code_ == kOk; }
    bool IsNotFound() const { return code_ == kNotFound; }
    bool IsCorruption() const { return code_ == kCorruption; }

    std::string ToString() const {
        if (code_ == kOk) return "OK";
        const char* names[] = {"OK", "NotFound", "Corruption", "IOError", "InvalidArgument"};
        return std::string(names[code_]) + (msg_.empty() ? "" : (" : " + msg_));
    }

private:
    enum Code { kOk = 0, kNotFound = 1, kCorruption = 2, kIOError = 3, kInvalidArgument = 4 };

    Status(Code code, const std::string& msg) : code_(code), msg_(msg) {}

    Code code_;
    std::string msg_;
};

#endif  // LDB_STATUS_H