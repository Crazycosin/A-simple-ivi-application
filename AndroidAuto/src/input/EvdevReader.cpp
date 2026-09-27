#include "input/EvdevReader.h"

#include <cerrno>
#include <cstring>

#include <glib-unix.h>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <linux/input.h>

namespace aa {
namespace {

#define TEST_BIT(bits, bit) \
    ((bits)[(bit) / (8 * sizeof(*(bits)))] >> \
     ((bit) % (8 * sizeof(*(bits)))) & 1)

bool getBits(int fd, unsigned int type, unsigned long* bits, std::size_t size) {
    return ::ioctl(fd, EVIOCGBIT(type, size), bits) >= 0;
}

} // namespace

EvdevReader::~EvdevReader() {
    stop();
}

bool EvdevReader::start(Log& log, const Options& opts, PointerFn onPointer,
                        MoveFn onMove) {
    stop();
    log_ = &log;
    onPointer_ = std::move(onPointer);
    onMove_ = std::move(onMove);
    screenW_ = opts.screenW;
    screenH_ = opts.screenH;
    x_ = opts.screenW / 2;  // 相对位移起点：屏幕中心（weston 光标同源
    y_ = opts.screenH / 2;  // 事件流，各自累积理论一致）

    bool any = false;
    for (int i = 0; i < 64; ++i) {
        const std::string path =
            opts.basePath + "/event" + std::to_string(i);
        const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            continue;  // 节点不存在（编号不连续属正常）
        }
        if (!isMouse(fd)) {
            ::close(fd);
            continue;
        }
        Dev d;
        d.fd = fd;
        d.source = g_unix_fd_add(fd, G_IO_IN, &EvdevReader::readableThunk, this);
        if (d.source == 0) {
            ::close(fd);
            continue;
        }
        devs_.push_back(d);
        any = true;
        log.write("[evdev] listening %s", path.c_str());
    }
    if (!any) {
        log.write("[evdev] no mouse-capable device under %s", 
                  opts.basePath.c_str());
    }
    return any;
}

void EvdevReader::stop() {
    for (auto& d : devs_) {
        if (d.source != 0) {
            g_source_remove(d.source);
        }
        if (d.fd >= 0) {
            ::close(d.fd);
        }
    }
    devs_.clear();
    log_ = nullptr;
    onPointer_ = nullptr;
    onMove_ = nullptr;
}

bool EvdevReader::isMouse(int fd) const {
    unsigned long evBits[(EV_MAX - 1) / (8 * sizeof(unsigned long)) + 1] = {};
    if (!getBits(fd, 0, evBits, sizeof evBits)) {
        return false;
    }
    if (!TEST_BIT(evBits, EV_REL) || !TEST_BIT(evBits, EV_KEY)) {
        return false;
    }
    unsigned long relBits[(REL_MAX - 1) / (8 * sizeof(unsigned long)) + 1] = {};
    if (!getBits(fd, EV_REL, relBits, sizeof relBits)) {
        return false;
    }
    if (!TEST_BIT(relBits, REL_X) || !TEST_BIT(relBits, REL_Y)) {
        return false;
    }
    unsigned long keyBits[(KEY_MAX - 1) / (8 * sizeof(unsigned long)) + 1] = {};
    if (!getBits(fd, EV_KEY, keyBits, sizeof keyBits)) {
        return false;
    }
    return TEST_BIT(keyBits, BTN_LEFT) || TEST_BIT(keyBits, BTN_RIGHT);
}

gboolean EvdevReader::readableThunk(gint fd, GIOCondition cond, gpointer data) {
    auto* self = static_cast<EvdevReader*>(data);
    if (cond & (G_IO_HUP | G_IO_ERR)) {
        return G_SOURCE_REMOVE;
    }
    self->onReadable(fd);
    return G_SOURCE_CONTINUE;
}

void EvdevReader::onReadable(int fd) {
    // 非阻塞读：evdev 一次 read 返回整数个 input_event
    struct input_event ev;
    while (::read(fd, &ev, sizeof ev) == sizeof ev) {
        switch (ev.type) {
        case EV_REL:
            if (ev.code == REL_X) {
                x_ += ev.value;
            } else if (ev.code == REL_Y) {
                y_ += ev.value;
            }
            // 坐标夹取屏幕范围
            if (screenW_ > 0) {
                if (x_ < 0) x_ = 0;
                if (x_ >= screenW_) x_ = screenW_ - 1;
            }
            if (screenH_ > 0) {
                if (y_ < 0) y_ = 0;
                if (y_ >= screenH_) y_ = screenH_ - 1;
            }
            if (onMove_) {
                onMove_(x_, y_);
            }
            break;
        case EV_KEY:
            if (ev.code == BTN_LEFT || ev.code == BTN_RIGHT) {
                // 仅按下沿回调（up 不重复上报，坐标无变化无信息量）
                if (ev.value == 1 && onPointer_) {
                    onPointer_(x_, y_,
                               ev.code == BTN_LEFT ? 0 : 1, true);
                }
            }
            break;
        default:
            break;  // EV_SYN/EV_MSC 等不关心
        }
    }
}

} // namespace aa
