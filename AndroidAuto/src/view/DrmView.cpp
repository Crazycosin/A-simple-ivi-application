#include "view/DrmView.h"

#include <cerrno>
#include <cstring>

#include <fcntl.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

namespace aa {
namespace {

/// 无效 modifier（fourcc_mod_code(NONE, DRM_FORMAT_RESERVED)）：
/// AddFB 不接受该值，遇到时退回无修饰路径。
constexpr std::uint64_t kModifierInvalid = 0x00ffffffffffffffULL;

} // namespace

DrmView::~DrmView() {
    destroy();
}

bool DrmView::create(Log& log) {
    Options opts;
    return create(log, opts);
}

bool DrmView::create(Log& log, const Options& opts) {
    destroy();
    log_ = &log;
    wantFourcc_ = opts.fourcc != 0 ? opts.fourcc
                                   : static_cast<std::uint32_t>(DRM_FORMAT_NV12);
    planeOverride_ = opts.planeId;

    devFd_ = ::open(opts.device.c_str(), O_RDWR | O_CLOEXEC);
    if (devFd_ < 0) {
        log.write("[drm] open %s failed: %s", opts.device.c_str(),
                  std::strerror(errno));
        return false;
    }
    if (!locateActiveCrtc()) {
        destroy();
        return false;
    }
    if (!pickPlane()) {
        destroy();
        return false;
    }
    if (!setZpos(2)) {
        // zpos 失败非致命：仍可显示，但可能与 weston primary 同层交替
        log.write("[drm] WARNING: zpos set failed, expect z-fighting flicker");
    }
    // pixel blend mode → None(2)：overlay 默认 Pre-multiplied(0) 会把
    // 下层 weston 画面混进视频（实测"画面不纯净/像图层叠加"）；weston
    // primary 自身就是 None，视频层照做纯覆盖
    if (!setPlaneProp("pixel blend mode", 2)) {
        log.write("[drm] WARNING: pixel blend mode set failed, "
                  "expect blended (impure) picture");
    }
    log.write("[drm] ready: plane=%u crtc=%u fourcc=%c%c%c%c device=%s "
              "zpos=2 blend=none",
              planeId_, crtcId_,
              static_cast<char>(wantFourcc_ & 0xff),
              static_cast<char>((wantFourcc_ >> 8) & 0xff),
              static_cast<char>((wantFourcc_ >> 16) & 0xff),
              static_cast<char>((wantFourcc_ >> 24) & 0xff),
              opts.device.c_str());
    return true;
}

void DrmView::setPlaneIdOverride(std::uint32_t id) {
    planeOverride_ = id;
}

void DrmView::destroy() {
    clear();
    if (devFd_ >= 0 && planeId_ != 0) {
        // 恢复默认属性：不改永久状态，weston 将来若把该 plane 分配
        // 给其他客户端不受影响
        setZpos(0);
        setPlaneProp("pixel blend mode", 0);  // Pre-multiplied（驱动默认）
    }
    if (devFd_ >= 0) {
        ::close(devFd_);
        devFd_ = -1;
    }
    crtcId_ = 0;
    planeId_ = 0;
    log_ = nullptr;
}

void DrmView::setViewport(std::int32_t x, std::int32_t y,
                          std::int32_t w, std::int32_t h) {
    vpX_ = x;
    vpY_ = y;
    vpW_ = w;
    vpH_ = h;
}

bool DrmView::setImageData(const ImageData& img) {
    if (!valid()) {
        return false;
    }
    if (img.numPlanes <= 0 || img.numPlanes > 4 || img.width <= 0 ||
        img.height <= 0 || img.fd[0] < 0 || img.pitch[0] == 0) {
        if (log_ != nullptr) {
            log_->write("[drm] setImageData: bad descriptor "
                        "(planes=%d %dx%d pitch=%u)",
                        img.numPlanes, img.width, img.height, img.pitch[0]);
        }
        return false;
    }
    if (vpW_ <= 0 || vpH_ <= 0) {
        if (log_ != nullptr) {
            log_->write("[drm] setImageData: viewport not set");
        }
        return false;
    }

    FbEntry fb = importFb(img);
    if (fb.fbId == 0) {
        return false;
    }

    // 提交：src 为整帧（16.16 定点），dest 为屏幕目标矩形。
    if (drmModeSetPlane(devFd_, planeId_, crtcId_, fb.fbId, 0,
                        vpX_, vpY_,
                        static_cast<std::uint32_t>(vpW_),
                        static_cast<std::uint32_t>(vpH_),
                        0, 0,
                        static_cast<std::uint32_t>(img.width) << 16,
                        static_cast<std::uint32_t>(img.height) << 16) != 0) {
        if (log_ != nullptr) {
            log_->write("[drm] setPlane failed: %s", std::strerror(errno));
        }
        destroyFb(fb);
        return false;
    }

    // 双 fb 轮换：新帧已在 plane 上，此刻销毁上一帧是安全的；
    // 再保上一帧一拍（prev_），防 buffer pool 提前回收。
    destroyFb(prev_);
    prev_ = current_;
    current_ = fb;
    return true;
}

void DrmView::clear() {
    if (devFd_ >= 0 && planeId_ != 0) {
        // 只关本 plane（fb_id=0）；不 modeset、不动 primary/connector。
        drmModeSetPlane(devFd_, planeId_, crtcId_, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    }
    destroyFb(prev_);
    destroyFb(current_);
}

bool DrmView::hitTest(std::int32_t sx, std::int32_t sy) const {
    return vpW_ > 0 && vpH_ > 0 && sx >= vpX_ && sx < vpX_ + vpW_ &&
           sy >= vpY_ && sy < vpY_ + vpH_;
}

// ---------------------------------------------------------------------------
// 内部：资源定位与 fb 登记
// ---------------------------------------------------------------------------

bool DrmView::locateActiveCrtc() {
    drmModeResPtr res = drmModeGetResources(devFd_);
    if (res == nullptr) {
        if (log_ != nullptr) {
            log_->write("[drm] getResources failed: %s", std::strerror(errno));
        }
        return false;
    }

    // 优先取“已连接 connector 的编码器当前绑定的 crtc”——那是 weston
    // 正在点亮的输出，overlay 必须挂同一个 crtc 才能同屏显示。
    for (int i = 0; i < res->count_connectors && crtcId_ == 0; ++i) {
        drmModeConnectorPtr conn = drmModeGetConnector(devFd_, res->connectors[i]);
        if (conn == nullptr) {
            continue;
        }
        if (conn->connection == DRM_MODE_CONNECTED && conn->encoder_id != 0) {
            drmModeEncoderPtr enc = drmModeGetEncoder(devFd_, conn->encoder_id);
            if (enc != nullptr) {
                crtcId_ = enc->crtc_id;
                drmModeFreeEncoder(enc);
            }
        }
        drmModeFreeConnector(conn);
    }
    const bool haveCrtc = crtcId_ != 0;
    if (!haveCrtc && log_ != nullptr) {
        log_->write("[drm] no active crtc on connected connector");
    }
    drmModeFreeResources(res);
    return haveCrtc;
}

bool DrmView::pickPlane() {
    // 强制指定（排障用）：跳过 format/占用检查，直接使用。
    if (planeOverride_ != 0) {
        drmModePlanePtr pl = drmModeGetPlane(devFd_, planeOverride_);
        if (pl == nullptr) {
            if (log_ != nullptr) {
                log_->write("[drm] forced plane %u not found: %s",
                            planeOverride_, std::strerror(errno));
            }
            return false;
        }
        drmModeFreePlane(pl);
        planeId_ = planeOverride_;
        if (log_ != nullptr) {
            log_->write("[drm] plane forced to %u (occupancy check skipped)",
                        planeId_);
        }
        return true;
    }

    drmModePlaneResPtr pres = drmModeGetPlaneResources(devFd_);
    if (pres == nullptr) {
        if (log_ != nullptr) {
            log_->write("[drm] getPlaneResources failed: %s",
                        std::strerror(errno));
        }
        return false;
    }

    bool found = false;
    for (std::uint32_t i = 0; i < pres->count_planes && !found; ++i) {
        drmModePlanePtr pl = drmModeGetPlane(devFd_, pres->planes[i]);
        if (pl == nullptr) {
            continue;
        }

        // 条件 1：format 能力匹配（plane 支持目标 fourcc）。
        bool fmtOk = false;
        for (std::uint32_t k = 0; k < pl->count_formats; ++k) {
            if (pl->formats[k] == wantFourcc_) {
                fmtOk = true;
                break;
            }
        }
        // 条件 2：plane 可用于当前 crtc（possible_crtcs 位图）。
        bool crtcOk = false;
        if (fmtOk) {
            drmModeResPtr res = drmModeGetResources(devFd_);
            if (res != nullptr) {
                for (int c = 0; c < res->count_crtcs; ++c) {
                    if (res->crtcs[c] == crtcId_) {
                        crtcOk = (pl->possible_crtcs & (1u << c)) != 0;
                        break;
                    }
                }
                drmModeFreeResources(res);
            }
        }

        // 条件 3：plane 类型与占用过滤。
        // drm_plane_type：Overlay=0 / Primary=1 / Cursor=2（内核 uapi）。
        // Primary/Cursor 永不碰——primary 是 weston 必然的输出层；其
        // FB_ID/CRTC_ID 在 atomic 提交语义下可能读到 0，type 过滤更稳。
        // FB_ID/CRTC_ID 非零 = 正被某客户端使用，同样跳过。
        bool free = false;
        std::uint64_t typeVal = 0;  // 默认按 Overlay 处理（无 type property）
        if (fmtOk && crtcOk) {
            drmModeObjectPropertiesPtr props =
                drmModeObjectGetProperties(devFd_, pl->plane_id,
                                           DRM_MODE_OBJECT_PLANE);
            if (props != nullptr) {
                free = true;
                std::uint64_t fbId = 0, crtcIdVal = 0;
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
                if (typeVal == 1 || typeVal == 2) {
                    free = false;  // primary/cursor：weston 体系内一律不碰
                    if (log_ != nullptr) {
                        log_->write("[drm] plane %u is %s, skip",
                                    pl->plane_id,
                                    typeVal == 1 ? "primary" : "cursor");
                    }
                } else if (fbId != 0 || crtcIdVal != 0) {
                    free = false;
                    if (log_ != nullptr) {
                        log_->write("[drm] plane %u busy (FB=%llu CRTC=%llu), "
                                    "skip",
                                    pl->plane_id,
                                    static_cast<unsigned long long>(fbId),
                                    static_cast<unsigned long long>(crtcIdVal));
                    }
                } else if (log_ != nullptr) {
                    // 诊断：候选空闲 overlay（观测用，确认选择依据）
                    log_->write("[drm] plane %u free overlay (type=%llu)",
                                pl->plane_id,
                                static_cast<unsigned long long>(typeVal));
                }
                drmModeFreeObjectProperties(props);
            }
        }

        if (fmtOk && crtcOk && free) {
            planeId_ = pl->plane_id;
            found = true;
        }
        drmModeFreePlane(pl);
    }
    drmModeFreePlaneResources(pres);

    if (!found && log_ != nullptr) {
        log_->write("[drm] no free overlay plane supports fourcc=0x%08x "
                    "on crtc=%u", wantFourcc_, crtcId_);
    }
    return found;
}

bool DrmView::setPlaneProp(const char* name, std::uint64_t value) {
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
            if (!ok && log_ != nullptr) {
                log_->write("[drm] prop '%s'=%llu set failed: %s", name,
                            static_cast<unsigned long long>(value),
                            std::strerror(errno));
            }
            drmModeFreeProperty(prop);
            break;
        }
        drmModeFreeProperty(prop);
    }
    drmModeFreeObjectProperties(props);
    return ok;
}

bool DrmView::setZpos(std::uint64_t z) {
    return setPlaneProp("zpos", z);
}

DrmView::FbEntry DrmView::importFb(const ImageData& img) {
    FbEntry e;
    std::uint32_t handles[4] = {0, 0, 0, 0};

    // fd → GEM handle。同一 fd 重复导入返回同一 handle 且引用计数
    // 递增，销毁时逐次 GEM_CLOSE 即可平衡。
    for (int p = 0; p < img.numPlanes; ++p) {
        if (img.fd[p] < 0) {
            if (log_ != nullptr) {
                log_->write("[drm] plane %d fd invalid", p);
            }
            return e;
        }
        std::uint32_t handle = 0;
        if (drmPrimeFDToHandle(devFd_, img.fd[p], &handle) != 0) {
            if (log_ != nullptr) {
                log_->write("[drm] prime fd->handle failed (plane %d): %s",
                            p, std::strerror(errno));
            }
            for (int q = 0; q < p; ++q) {
                drm_ioctl_gem_close(handles[q]);
            }
            return e;
        }
        handles[p] = handle;
        e.gemHandles.push_back(handle);
    }

    const std::uint64_t mod = img.modifier;
    int ret = -1;
    if (mod != 0 && mod != kModifierInvalid &&
        mod != DRM_FORMAT_MOD_INVALID) {
        std::uint64_t mods[4] = {mod, mod, mod, mod};
        ret = drmModeAddFB2WithModifiers(
            devFd_, img.width, img.height, img.fourcc, handles,
            const_cast<std::uint32_t*>(img.pitch),
            const_cast<std::uint32_t*>(img.offset), mods, &e.fbId,
            DRM_MODE_FB_MODIFIERS);
        if (ret != 0 && log_ != nullptr) {
            log_->write("[drm] addFB2WithModifiers mod=0x%llx rejected: %s "
                        "(retry linear)",
                        static_cast<unsigned long long>(mod),
                        std::strerror(errno));
        }
    }
    if (ret != 0) {
        // 回退 LINEAR（modifier 被拒/未配置）：tiled 数据按 linear 采样
        // 会脏画面，但至少有画面 + 日志可查
        ret = drmModeAddFB2(devFd_, img.width, img.height, img.fourcc,
                            handles, const_cast<std::uint32_t*>(img.pitch),
                            const_cast<std::uint32_t*>(img.offset),
                            &e.fbId, 0);
    }
    if (ret != 0) {
        if (log_ != nullptr) {
            log_->write("[drm] addFB2 failed (fourcc=0x%08x mod=0x%llx): %s",
                        img.fourcc,
                        static_cast<unsigned long long>(mod),
                        std::strerror(errno));
        }
        e.fbId = 0;
    }

    if (e.fbId == 0) {
        for (auto h : e.gemHandles) {
            drm_ioctl_gem_close(h);
        }
        e.gemHandles.clear();
    }
    return e;
}

void DrmView::destroyFb(FbEntry& e) {
    if (e.fbId != 0) {
        drmModeRmFB(devFd_, e.fbId);
        e.fbId = 0;
    }
    for (auto h : e.gemHandles) {
        drm_ioctl_gem_close(h);
    }
    e.gemHandles.clear();
}

// ---------------------------------------------------------------------------
// 私有工具
// ---------------------------------------------------------------------------

void DrmView::drm_ioctl_gem_close(std::uint32_t handle) {
    if (handle == 0) {
        return;
    }
    drm_gem_close args{};
    args.handle = handle;
    // best-effort：句柄异常时仅丢弃（设备关闭时内核统一回收）。
    drmIoctl(devFd_, DRM_IOCTL_GEM_CLOSE, &args);
}

} // namespace aa
