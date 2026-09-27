#include "util/CursorOverlay.h"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cairo.h>
#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

namespace aa {
namespace {

constexpr std::int32_t kCursorW = 20;
constexpr std::int32_t kCursorH = 28;

} // namespace

CursorOverlay::~CursorOverlay() {
    destroy();
}

bool CursorOverlay::create(Log& log, const std::string& device,
                            std::uint32_t crtcId, std::uint32_t avoidPlaneId) {
    destroy();
    log_ = &log;
    crtcId_ = crtcId;
    w_ = kCursorW;
    h_ = kCursorH;

    devFd_ = ::open(device.c_str(), O_RDWR | O_CLOEXEC);
    if (devFd_ < 0) {
        log.write("[cursor] open %s failed: %s", device.c_str(),
                  std::strerror(errno));
        return false;
    }
    if (!pickPlane(avoidPlaneId)) {
        destroy();
        return false;
    }

    // dumb buffer：光标是静态小位图，不走 dmabuf/GStreamer，直接用最
    // 简单的 KMS dumb buffer（create+mmap+画+addfb），比照搬 DrmView
    // 那套 dmabuf 导入逻辑省事得多，也不掺和视频那条 zero-copy 链路。
    struct drm_mode_create_dumb creq {};
    creq.width = static_cast<std::uint32_t>(w_);
    creq.height = static_cast<std::uint32_t>(h_);
    creq.bpp = 32;
    if (drmIoctl(devFd_, DRM_IOCTL_MODE_CREATE_DUMB, &creq) != 0) {
        log.write("[cursor] create dumb buffer failed: %s",
                  std::strerror(errno));
        destroy();
        return false;
    }
    gemHandle_ = creq.handle;
    pitch_ = creq.pitch;

    struct drm_mode_map_dumb mreq {};
    mreq.handle = gemHandle_;
    if (drmIoctl(devFd_, DRM_IOCTL_MODE_MAP_DUMB, &mreq) != 0) {
        log.write("[cursor] map dumb buffer failed: %s", std::strerror(errno));
        destroy();
        return false;
    }
    mappedSize_ = creq.size;
    mapped_ = ::mmap(nullptr, mappedSize_, PROT_READ | PROT_WRITE, MAP_SHARED,
                     devFd_, static_cast<off_t>(mreq.offset));
    if (mapped_ == MAP_FAILED) {
        log.write("[cursor] mmap dumb buffer failed: %s", std::strerror(errno));
        mapped_ = nullptr;
        destroy();
        return false;
    }
    std::memset(mapped_, 0, mappedSize_);  // 全透明底

    drawArrow();

    std::uint32_t handles[4] = {gemHandle_, 0, 0, 0};
    std::uint32_t pitches[4] = {pitch_, 0, 0, 0};
    std::uint32_t offsets[4] = {0, 0, 0, 0};
    if (drmModeAddFB2(devFd_, static_cast<std::uint32_t>(w_),
                      static_cast<std::uint32_t>(h_), DRM_FORMAT_ARGB8888,
                      handles, pitches, offsets, &fbId_, 0) != 0) {
        log.write("[cursor] addFB2 failed: %s", std::strerror(errno));
        destroy();
        return false;
    }

    if (!setPlaneProp("zpos", 3)) {
        log.write("[cursor] WARNING: zpos set failed, may render below video");
    }
    // pixel blend mode 保持驱动默认 Pre-multiplied：箭头图标带透明边缘，
    // 需要正常跟下层混合——跟 DrmView 视频 plane 特意关混合（那边要求
    // 纯覆盖不透光）刚好相反，不用碰这个属性。

    curX_ = 0;
    curY_ = 0;
    if (drmModeSetPlane(devFd_, planeId_, crtcId_, fbId_, 0, curX_, curY_,
                        static_cast<std::uint32_t>(w_),
                        static_cast<std::uint32_t>(h_), 0, 0,
                        static_cast<std::uint32_t>(w_) << 16,
                        static_cast<std::uint32_t>(h_) << 16) != 0) {
        log.write("[cursor] initial setPlane failed: %s", std::strerror(errno));
        destroy();
        return false;
    }
    log.write("[cursor] ready: plane=%u zpos=3 size=%dx%d", planeId_, w_, h_);
    return true;
}

void CursorOverlay::destroy() {
    if (flushSource_ != 0) {
        g_source_remove(flushSource_);
        flushSource_ = 0;
    }
    hasPending_ = false;
    if (devFd_ >= 0 && planeId_ != 0) {
        drmModeSetPlane(devFd_, planeId_, crtcId_, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        setPlaneProp("zpos", 0);
    }
    if (fbId_ != 0) {
        drmModeRmFB(devFd_, fbId_);
        fbId_ = 0;
    }
    if (mapped_ != nullptr) {
        ::munmap(mapped_, mappedSize_);
        mapped_ = nullptr;
        mappedSize_ = 0;
    }
    if (gemHandle_ != 0 && devFd_ >= 0) {
        struct drm_mode_destroy_dumb dreq {};
        dreq.handle = gemHandle_;
        drmIoctl(devFd_, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
        gemHandle_ = 0;
    }
    if (devFd_ >= 0) {
        ::close(devFd_);
        devFd_ = -1;
    }
    crtcId_ = 0;
    planeId_ = 0;
    log_ = nullptr;
}

void CursorOverlay::moveTo(std::int32_t screenX, std::int32_t screenY) {
    if (!valid()) {
        return;
    }
    pendingX_ = screenX;
    pendingY_ = screenY;
    hasPending_ = true;

    // 节流（2026-09-18 实测发现的问题）：legacy drmModeSetPlane 是同步
    // ioctl，且和 DrmView 的视频 plane 提交共用同一个 crtcId_——内核对
    // 同一 CRTC 的 plane 提交是串行化的，很多驱动的 legacy SetPlane 还会
    // 等到下一个 vblank 才返回。evdev 原始移动事件率可到几百 Hz，远超
    // 屏幕刷新率，逐事件同步提交会在主线程（onMove 回调与视频帧拉取/
    // 提交同在 glib 主循环）里排队等锁，表现为"移动光标时视频卡顿"。
    // 这里按显示帧率上限节流：窗口内只记录最新目标坐标，用一次性定时器
    // 补提交最后位置，不丢最终坐标。
    constexpr gint64 kMinIntervalUs = 16000;  // ~60Hz，对齐一帧
    const gint64 now = g_get_monotonic_time();
    if (now - lastCommitUs_ < kMinIntervalUs) {
        if (flushSource_ == 0) {
            const guint delayMs = static_cast<guint>(
                (kMinIntervalUs - (now - lastCommitUs_) + 999) / 1000);
            flushSource_ = g_timeout_add(delayMs, &CursorOverlay::flushThunk, this);
        }
        return;
    }
    commit(screenX, screenY);
}

gboolean CursorOverlay::flushThunk(gpointer data) {
    static_cast<CursorOverlay*>(data)->onFlushTimer();
    return G_SOURCE_REMOVE;
}

void CursorOverlay::onFlushTimer() {
    flushSource_ = 0;
    if (hasPending_) {
        commit(pendingX_, pendingY_);
    }
}

void CursorOverlay::commit(std::int32_t x, std::int32_t y) {
    hasPending_ = false;
    lastCommitUs_ = g_get_monotonic_time();
    if (x == curX_ && y == curY_) {
        return;  // 没动，省一次 ioctl
    }
    curX_ = x;
    curY_ = y;
    drmModeSetPlane(devFd_, planeId_, crtcId_, fbId_, 0, curX_, curY_,
                    static_cast<std::uint32_t>(w_),
                    static_cast<std::uint32_t>(h_), 0, 0,
                    static_cast<std::uint32_t>(w_) << 16,
                    static_cast<std::uint32_t>(h_) << 16);
}

// ---------------------------------------------------------------------------
// 内部
// ---------------------------------------------------------------------------

bool CursorOverlay::pickPlane(std::uint32_t avoidPlaneId) {
    drmModePlaneResPtr pres = drmModeGetPlaneResources(devFd_);
    if (pres == nullptr) {
        if (log_ != nullptr) {
            log_->write("[cursor] getPlaneResources failed: %s",
                        std::strerror(errno));
        }
        return false;
    }

    drmModeResPtr res = drmModeGetResources(devFd_);
    int crtcBit = -1;
    if (res != nullptr) {
        for (int c = 0; c < res->count_crtcs; ++c) {
            if (res->crtcs[c] == crtcId_) {
                crtcBit = c;
                break;
            }
        }
        drmModeFreeResources(res);
    }

    bool found = false;
    for (std::uint32_t i = 0; i < pres->count_planes && !found; ++i) {
        const std::uint32_t id = pres->planes[i];
        if (id == avoidPlaneId) {
            continue;  // 视频占用的那块——首帧提交前占用检测看不出来
        }
        drmModePlanePtr pl = drmModeGetPlane(devFd_, id);
        if (pl == nullptr) {
            continue;
        }

        bool fmtOk = false;
        for (std::uint32_t k = 0; k < pl->count_formats; ++k) {
            if (pl->formats[k] == static_cast<std::uint32_t>(DRM_FORMAT_ARGB8888)) {
                fmtOk = true;
                break;
            }
        }
        const bool crtcOk =
            crtcBit >= 0 && (pl->possible_crtcs & (1u << crtcBit)) != 0;

        bool free = false;
        if (fmtOk && crtcOk) {
            drmModeObjectPropertiesPtr props =
                drmModeObjectGetProperties(devFd_, id, DRM_MODE_OBJECT_PLANE);
            if (props != nullptr) {
                free = true;
                std::uint64_t typeVal = 0, fbId = 0, crtcIdVal = 0;
                for (std::uint32_t p = 0; p < props->count_props; ++p) {
                    drmModePropertyPtr prop =
                        drmModeGetProperty(devFd_, props->props[p]);
                    if (prop == nullptr) {
                        continue;
                    }
                    const std::uint64_t val = props->prop_values[p];
                    if (std::strcmp(prop->name, "type") == 0) {
                        typeVal = val;
                    } else if (std::strcmp(prop->name, "FB_ID") == 0) {
                        fbId = val;
                    } else if (std::strcmp(prop->name, "CRTC_ID") == 0) {
                        crtcIdVal = val;
                    }
                    drmModeFreeProperty(prop);
                }
                // drm_plane_type：Overlay=0/Primary=1/Cursor=2，
                // primary/cursor 一律不碰（同 DrmView 的规则）。
                if (typeVal == 1 || typeVal == 2) {
                    free = false;
                } else if (fbId != 0 || crtcIdVal != 0) {
                    free = false;
                }
                drmModeFreeObjectProperties(props);
            }
        }

        if (fmtOk && crtcOk && free) {
            planeId_ = id;
            found = true;
        }
        drmModeFreePlane(pl);
    }
    drmModeFreePlaneResources(pres);

    if (!found && log_ != nullptr) {
        log_->write("[cursor] no free ARGB8888 overlay plane on crtc=%u "
                    "(video plane=%u excluded)——skipping cursor overlay",
                    crtcId_, avoidPlaneId);
    }
    return found;
}

bool CursorOverlay::setPlaneProp(const char* name, std::uint64_t value) {
    if (devFd_ < 0 || planeId_ == 0) {
        return false;
    }
    drmModeObjectPropertiesPtr props =
        drmModeObjectGetProperties(devFd_, planeId_, DRM_MODE_OBJECT_PLANE);
    if (props == nullptr) {
        return false;
    }
    bool ok = false;
    for (std::uint32_t p = 0; p < props->count_props; ++p) {
        drmModePropertyPtr prop = drmModeGetProperty(devFd_, props->props[p]);
        if (prop == nullptr) {
            continue;
        }
        if (std::strcmp(prop->name, name) == 0) {
            ok = drmModeObjectSetProperty(devFd_, planeId_,
                                          DRM_MODE_OBJECT_PLANE,
                                          props->props[p], value) == 0;
            drmModeFreeProperty(prop);
            break;
        }
        drmModeFreeProperty(prop);
    }
    drmModeFreeObjectProperties(props);
    return ok;
}

void CursorOverlay::drawArrow() {
    cairo_surface_t* surf = cairo_image_surface_create_for_data(
        static_cast<unsigned char*>(mapped_), CAIRO_FORMAT_ARGB32, w_, h_,
        static_cast<int>(pitch_));
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
        if (log_ != nullptr) {
            log_->write("[cursor] cairo surface create failed (status=%d)",
                        cairo_surface_status(surf));
        }
        cairo_surface_destroy(surf);
        return;
    }
    cairo_t* cr = cairo_create(surf);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);  // 直接写像素含 alpha

    // 简单箭头：尖端在 (0,0)（= moveTo 传入的屏幕坐标），黑边白底，
    // 跟常见系统指针观感一致。
    cairo_move_to(cr, 0, 0);
    cairo_line_to(cr, 0, h_ * 0.75);
    cairo_line_to(cr, w_ * 0.35, h_ * 0.55);
    cairo_line_to(cr, w_ * 0.58, h_ * 0.98);
    cairo_line_to(cr, w_ * 0.75, h_ * 0.88);
    cairo_line_to(cr, w_ * 0.45, h_ * 0.42);
    cairo_line_to(cr, w_ * 0.78, h_ * 0.42);
    cairo_close_path(cr);

    cairo_set_source_rgba(cr, 1, 1, 1, 1);
    cairo_fill_preserve(cr);
    cairo_set_source_rgba(cr, 0, 0, 0, 1);
    cairo_set_line_width(cr, 1.6);
    cairo_stroke(cr);

    cairo_destroy(cr);
    cairo_surface_flush(surf);
    cairo_surface_destroy(surf);
}

} // namespace aa
