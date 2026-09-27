#pragma once

#include <cairo.h>

#include <cstdint>

#include "model/AppConfig.h"
#include "model/AppModel.h"

namespace aa {

/// 当前视频帧的只读视图（数据所有权在 App/VideoPlayer，
/// 一次 repaint 期间有效）
struct VideoFrameView {
    const std::uint8_t* data = nullptr;  ///< BGRx（与 cairo RGB24 内存序一致）
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t stride = 0;
};

/// View：cairo 绘制。只读 Config / Model，不持有 wayland 资源。
/// 所有尺寸、颜色、文案均来自 Config（ini），代码内无 UI 硬编码。
class AppView {
public:
    explicit AppView(const Config& cfg);
    ~AppView();
    AppView(const AppView&) = delete;
    AppView& operator=(const AppView&) = delete;

    /// frame 为 nullptr 或 data 为空时画占位窗口
    void paint(cairo_t* cr, const AppModel& model,
               const VideoFrameView* frame = nullptr);

private:
    void drawBackground(cairo_t* cr, std::int32_t w, std::int32_t h) const;
    void drawTitleBar(cairo_t* cr, std::int32_t winW) const;
    void drawCloseButton(cairo_t* cr, std::int32_t winW) const;
    void drawVideoWindow(cairo_t* cr, std::int32_t winW, std::int32_t winH,
                         const VideoFrameView* frame) const;
    bool ensureLogo() const;

    const Config& cfg_;
    mutable cairo_surface_t* logo_ = nullptr;  // 懒加载
    mutable bool logoFailed_ = false;
};

} // namespace aa
