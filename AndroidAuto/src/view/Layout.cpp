#include "view/Layout.h"

namespace aa {

Rect Layout::titleBar(const Config& c, std::int32_t winW) {
    return Rect{0, 0, winW, c.ui.titleBarHeight};
}

Rect Layout::closeHit(const Config& c, std::int32_t winW) {
    const auto s = static_cast<std::int32_t>(c.ui.closeButtonSize);
    const auto m = static_cast<std::int32_t>(c.ui.closeButtonMargin);
    return Rect{winW - s - m, m, s, s};
}

Rect Layout::contentArea(const Config& c, std::int32_t winW, std::int32_t winH) {
    return Rect{0, c.ui.titleBarHeight, winW, winH - c.ui.titleBarHeight};
}

Rect Layout::videoWindow(const Config& c, std::int32_t winW,
                         std::int32_t winH, std::int32_t aspectW,
                         std::int32_t aspectH) {
    // ini 显式配置的宽高比优先；否则用调用方给的（视频流实际比例）
    std::int64_t aw = c.ui.videoAspectW;
    std::int64_t ah = c.ui.videoAspectH;
    if (aw == 0 || ah == 0) {
        aw = aspectW;
        ah = aspectH;
    }
    const auto m = static_cast<std::int32_t>(c.ui.videoMargin);
    const Rect area = contentArea(c, winW, winH);

    const std::int64_t cw = area.w - 2 * static_cast<std::int64_t>(m);
    const std::int64_t ch = area.h - 2 * static_cast<std::int64_t>(m);
    if (aw == 0 || ah == 0 || cw <= 0 || ch <= 0) {
        return Rect{};
    }

    std::int64_t w = 0;
    std::int64_t h = 0;
    if (cw * ah <= ch * aw) {
        w = cw;                    // 宽度受限
        h = cw * ah / aw;
    } else {
        h = ch;                    // 高度受限
        w = ch * aw / ah;
    }
    return Rect{area.x + static_cast<std::int32_t>((area.w - w) / 2),
                area.y + static_cast<std::int32_t>((area.h - h) / 2),
                static_cast<std::int32_t>(w),
                static_cast<std::int32_t>(h)};
}

} // namespace aa
