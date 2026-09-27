#pragma once

#include <cstdint>

#include "model/AppConfig.h"

namespace aa {

/// 矩形（surface 本地坐标）
struct Rect {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::int32_t w = 0;
    std::int32_t h = 0;

    bool contains(std::int32_t px, std::int32_t py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

/// 纯几何计算（无状态、无绘制、可测试）
class Layout {
public:
    static Rect titleBar(const Config& c, std::int32_t winW);
    static Rect closeHit(const Config& c, std::int32_t winW);
    static Rect contentArea(const Config& c, std::int32_t winW, std::int32_t winH);
    /// 播放窗口：内容区内 video_margin 内接的 aspectW:aspectH 矩形。
    /// aspectW/H 传 0/0 = 无视频（按 ini 的 video_aspect_w/h 配置）。
    /// 有视频流时调用方通常传视频实际宽高比（ini 显式配置优先）。
    static Rect videoWindow(const Config& c, std::int32_t winW,
                            std::int32_t winH, std::int32_t aspectW = 0,
                            std::int32_t aspectH = 0);
};

} // namespace aa
