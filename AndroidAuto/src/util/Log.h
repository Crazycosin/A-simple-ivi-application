#pragma once

#include <cstdio>
#include <string>

namespace aa {

/// RAII 文件日志（追加写）。open 失败自动降级到 stderr。
/// move-only：所有权可转移，禁止拷贝。
class Log {
public:
    Log() = default;
    ~Log();
    Log(const Log&) = delete;
    Log& operator=(const Log&) = delete;
    Log(Log&& other) noexcept;
    Log& operator=(Log&&) noexcept;

    bool open(const std::string& path);
    void write(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
    bool valid() const { return file_ != nullptr; }

private:
    void closeOwned();

    std::FILE* file_ = nullptr;
    bool owns_ = false;
};

} // namespace aa
