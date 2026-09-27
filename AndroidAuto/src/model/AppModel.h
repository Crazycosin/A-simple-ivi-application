#pragma once

#include <cstdint>

namespace aa {

/// 应用运行时状态（Model）：仅 Controller 可写，View 只读。
class AppModel {
public:
    struct TouchPoint {
        std::int32_t x = 0;
        std::int32_t y = 0;
    };

    void setWindowSize(std::int32_t w, std::int32_t h);
    std::int32_t windowWidth() const { return width_; }
    std::int32_t windowHeight() const { return height_; }
    bool hasSize() const { return width_ > 0 && height_ > 0; }

    void quit() { running_ = false; }
    bool running() const { return running_; }

    /// dirty 标记：状态变化需要重绘（事件驱动，空闲零重绘）
    void markDirty() { dirty_ = true; }
    bool takeDirty() {
        const bool d = dirty_;
        dirty_ = false;
        return d;
    }

    void recordTouch(std::int32_t x, std::int32_t y) { lastTouch_ = {x, y}; }
    const TouchPoint& lastTouch() const { return lastTouch_; }

    /// 视频流实际尺寸（首帧到达时设置；驱动播放窗口宽高比自适应）
    void setVideoSize(std::int32_t w, std::int32_t h) {
        if (videoW_ != w || videoH_ != h) {
            videoW_ = w;
            videoH_ = h;
            markDirty();
        }
    }
    std::int32_t videoWidth() const { return videoW_; }
    std::int32_t videoHeight() const { return videoH_; }
    bool hasVideo() const { return videoW_ > 0 && videoH_ > 0; }

private:
    std::int32_t width_ = 0;
    std::int32_t height_ = 0;
    bool running_ = true;
    bool dirty_ = false;
    TouchPoint lastTouch_{};
    std::int32_t videoW_ = 0;
    std::int32_t videoH_ = 0;
};

} // namespace aa
