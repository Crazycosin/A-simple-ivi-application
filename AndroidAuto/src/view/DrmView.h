#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "util/Log.h"

namespace aa {

/// DRM 直扫视图（混合路线：视频帧经空闲 overlay plane 直接上屏）。
///
/// 对外接口与 wayland 提交路径同构，调用方（App/MediaPlayer 装配层）
/// 在 shm/cairo 路径与 DRM 路径之间切换时不需要改结构：
///
///   setViewport(x, y, w, h)   配置视频窗口在屏幕上的目标矩形
///                              （≈ wayland 侧的窗口/播放区几何）
///   setImageData(img)         提交一帧 ≈ wl_surface_attach +
///                              damage_buffer + commit 一步完成
///   clear()                   停止输出 ≈ setplane(fb_id=0)，仅关本 plane
///
/// 帧数据所有权在调用方（MediaPlayer 持有的 GstSample/GstBuffer）；
/// 本类只登记 fb 引用，换帧成功后延迟一帧销毁旧 fb（双 fb 轮换，
/// 保证 plane 不再扫描已归还的 buffer）。
///
/// plane 选择规则（实测依据 2026-09-18-dmabuf-zero-copy-route.md §7/§8）：
///   1. 枚举 plane 的 FB_ID/CRTC_ID property，非零（weston 正在使用）即跳过；
///   2. 只 setplane overlay，绝不 modeset、不碰 primary、不动 connector；
///   3. plane format 能力匹配（默认找支持 NV12 的 plane）。
///
/// RAII：析构等价 clear() + 关设备；destroy() 幂等；禁止拷贝。
class DrmView {
public:
    /// 一帧 dmabuf 图像的描述。fd 归调用方所有，本类只借用登记；
    /// 各 plane 可共用同一 fd（offset 区分）。
    struct ImageData {
        int fd[4] = {-1, -1, -1, -1};            ///< 每 plane 的 dmabuf fd
        std::uint32_t offset[4] = {0, 0, 0, 0};  ///< 每 plane 起始偏移
        std::uint32_t pitch[4] = {0, 0, 0, 0};   ///< 每 plane 行距
        int numPlanes = 0;                       ///< NV12 = 2
        std::uint32_t fourcc = 0;                ///< DRM_FORMAT_*（如 NV12）
        std::uint64_t modifier = 0;              ///< 0 = LINEAR/无修饰
        std::int32_t width = 0;
        std::int32_t height = 0;
    };

    struct Options {
        std::string device = "/dev/dri/card0";  ///< DRM 设备节点
        std::uint32_t fourcc = 0;               ///< 0 = DRM_FORMAT_NV12
        std::uint32_t planeId = 0;              ///< 0 = 自动挑选；非零 = 强制指定（排障用）
    };

    DrmView() = default;
    ~DrmView();
    DrmView(const DrmView&) = delete;
    DrmView& operator=(const DrmView&) = delete;

    /// 打开设备、定位活动 crtc、挑选空闲且 format 匹配的 overlay plane。
    /// 失败返回 false（原因进 log），之后 valid() 为 false。
    bool create(Log& log);
    bool create(Log& log, const Options& opts);

    // 已配置的强制 plane id（create 前设置；0 = 自动挑选）
    void setPlaneIdOverride(std::uint32_t id);
    void destroy();

    /// 屏幕目标矩形（视频窗口的屏幕绝对坐标）。播放中可更新（如
    /// configure 尺寸变化重算后调用）；仅影响后续 setImageData。
    void setViewport(std::int32_t x, std::int32_t y,
                     std::int32_t w, std::int32_t h);

    /// 提交一帧：AddFB2(WithModifiers) + drmModeSetPlane。
    /// 成功返回 true；旧 fb 延迟一帧销毁。失败返回 false 且不影响
    /// 当前正在扫描的帧。img 生命周期仅需覆盖本次调用。
    bool setImageData(const ImageData& img);

    /// 停止输出：setplane(fb=0) + 释放登记的全部 fb。
    /// weston 画面不受影响（不 modeset、不动 primary）。
    void clear();

    bool valid() const { return devFd_ >= 0 && planeId_ != 0; }
    std::int32_t viewportX() const { return vpX_; }
    std::int32_t viewportY() const { return vpY_; }
    std::int32_t viewportW() const { return vpW_; }
    std::int32_t viewportH() const { return vpH_; }
    std::uint32_t planeId() const { return planeId_; }
    std::uint32_t crtcId() const { return crtcId_; }  ///< CursorOverlay 复用同一 crtc

    /// 命中测试：屏幕坐标是否落在当前 viewport 内（evdev 坐标上报用）。
    bool hitTest(std::int32_t sx, std::int32_t sy) const;

private:
    struct FbEntry {
        std::uint32_t fbId = 0;
        std::vector<std::uint32_t> gemHandles;  // fb 依赖的 GEM handle
    };

    bool locateActiveCrtc();
    bool pickPlane();
    /// 按名设置 plane property（返回是否找到并成功）
    bool setPlaneProp(const char* name, std::uint64_t value);
    /// 设置/恢复 plane zpos（zpos 相同的 plane 扫描顺序未定义：实测与
    /// weston primary 同为 0 时视频/界面逐帧交替 = 闪屏；视频层须显式
    /// 抬高。范围 0-3，取 2：高于 weston primary(0)，低于顶层 RGB 层）
    bool setZpos(std::uint64_t z);
    FbEntry importFb(const ImageData& img);
    void destroyFb(FbEntry& e);
    void drm_ioctl_gem_close(std::uint32_t handle);  // GEM 句柄释放（best-effort）

    Log* log_ = nullptr;
    int devFd_ = -1;
    std::uint32_t crtcId_ = 0;
    std::uint32_t planeId_ = 0;
    std::uint32_t planeOverride_ = 0;  // 非零 = 强制 plane（跳过占用检查）
    std::uint32_t wantFourcc_ = 0;

    std::int32_t vpX_ = 0;
    std::int32_t vpY_ = 0;
    std::int32_t vpW_ = 0;
    std::int32_t vpH_ = 0;

    FbEntry current_;  // 正在 plane 上扫描的 fb
    FbEntry prev_;     // 上一帧 fb（延迟一帧销毁，双 fb 轮换）
};

} // namespace aa
