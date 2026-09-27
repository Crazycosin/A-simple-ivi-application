#pragma once

#include <cstddef>
#include <cstdint>

namespace aa {

/// 传输方式（screencast-avsync-design §6）：决定解码前 queue 深度与卡顿阈值。
/// USB / WiFi 近场热点两档；数值为设计文档经验值，可被 ini 覆盖。
enum class Transport : std::uint8_t {
    Usb = 0,
    Wifi,
};

struct TransportProfile {
    std::uint64_t preDecodeQueueMaxNs = 0;  // 解码前 queue 的 max-size-time
    std::uint64_t stallThresholdNs = 0;     // 卡顿判定阈值（queue 水位）
};

inline TransportProfile profileFor(Transport t) {
    switch (t) {
    case Transport::Wifi:
        return {500ull * 1000000ull, 300ull * 1000000ull};
    case Transport::Usb:
    default:
        return {100ull * 1000000ull, 60ull * 1000000ull};
    }
}

/// 解码后的视频帧描述。
/// shm 路径：data 为 BGRx（与 wl_shm/cairo RGB24 内存序一致），由
/// MediaPlayer 持有的 GstSample 保证有效：下一次 pullVideoFrame()
/// 或 stop() 之前可安全读取（主线程内串行使用）。
/// drm 路径：data 为空，isDmabuf=true，fd/offset/pitch/fourcc/modifier
/// 描述 dmabuf 帧（fd 归 GstBuffer 所有，同样仅在下次 pull/stop 前有效；
/// DRM 侧经 GEM handle 持有引用，plane 扫描期内存不释放）。
struct VideoFrame {
    const std::uint8_t* data = nullptr;  ///< BGRx（shm 路径有效）
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t stride = 0;
    std::int64_t ptsNs = -1;  ///< 已扣除 base_ts 的管线 PTS；-1 = 无 PTS

    // ---- dmabuf 路径（render_path=drm，omx use-dmabuf=true 输出）----
    bool isDmabuf = false;
    int fd[4] = {-1, -1, -1, -1};              ///< 每 plane 的 dmabuf fd（借用）
    std::uint32_t offset[4] = {0, 0, 0, 0};    ///< 每 plane 起始偏移（GstVideoMeta）
    std::uint32_t pitch[4] = {0, 0, 0, 0};     ///< 每 plane 行距（GstVideoMeta）
    int numPlanes = 0;                          ///< NV12 = 2
    std::uint32_t fourcc = 0;                   ///< DRM fourcc（DRM_FORMAT_NV12 等）
    std::uint64_t modifier = 0;                 ///< 0 = LINEAR/未声明
};

} // namespace aa
