#include "util/Log.h"

#include <cstdarg>
#include <ctime>

namespace aa {

Log::~Log() {
    closeOwned();
}

Log::Log(Log&& other) noexcept
    : file_(other.file_), owns_(other.owns_) {
    other.file_ = nullptr;
    other.owns_ = false;
}

Log& Log::operator=(Log&& other) noexcept {
    if (this != &other) {
        closeOwned();
        file_ = other.file_;
        owns_ = other.owns_;
        other.file_ = nullptr;
        other.owns_ = false;
    }
    return *this;
}

bool Log::open(const std::string& path) {
    closeOwned();
    if (path.empty()) {
        file_ = stderr;
        owns_ = false;
        return true;
    }
    std::FILE* f = std::fopen(path.c_str(), "a");
    if (f == nullptr) {
        file_ = stderr;
        owns_ = false;
        return false;
    }
    file_ = f;
    owns_ = true;
    return true;
}

void Log::write(const char* fmt, ...) {
    if (file_ == nullptr) {
        file_ = stderr;
    }
    std::time_t now = std::time(nullptr);
    char ts[16];
    std::strftime(ts, sizeof(ts), "%H:%M:%S", std::localtime(&now));
    std::fprintf(file_, "[%s] ", ts);

    std::va_list ap;
    va_start(ap, fmt);
    std::vfprintf(file_, fmt, ap);
    va_end(ap);
    std::fputc('\n', file_);
    std::fflush(file_);
}

void Log::closeOwned() {
    if (owns_ && file_ != nullptr) {
        std::fclose(file_);
    }
    file_ = nullptr;
    owns_ = false;
}

} // namespace aa
