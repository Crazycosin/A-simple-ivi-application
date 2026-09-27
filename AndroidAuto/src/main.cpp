/// AndroidAuto 入口：定位可执行文件目录 → 加载 ini → 装配 MVC → 主循环
#include <unistd.h>

#include <string>

#include "app/AndroidAutoApp.h"

namespace {

std::string exeDirectory() {
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) {
        return ".";
    }
    buf[n] = '\0';
    std::string path(buf);
    const auto pos = path.find_last_of('/');
    return (pos == std::string::npos) ? std::string(".") : path.substr(0, pos);
}

} // namespace

int main() {
    return aa::AndroidAutoApp::run(exeDirectory());
}
