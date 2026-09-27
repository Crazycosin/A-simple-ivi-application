#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <glib.h>

#include "util/Log.h"

namespace aa {

/// evdev 直读输入（混合路线 C 的视频区输入通道）。
///
/// 背景：视频帧在 DRM overlay plane 上时，weston 看不到该区域（它只
/// 感知自己的 surface 树），视频区点击事件不会派发给本应用。evdev
/// 设备节点支持多读者并存（每个 fd 独立事件队列，weston 与本进程
/// 各收一份完整流，互不干扰），故自行读取并换算屏幕坐标。
///
/// 现阶段只处理鼠标（REL_X/REL_Y + BTN_LEFT/BTN_RIGHT——板级触摸
/// 驱动未编译，见 x9hp_problems.md；将来触摸接入时按 ABS 设备扩展或
/// 换 libinput）。相对位移从屏幕中心起累积，夹取 [0, screenW/H)；
/// 与 weston 光标消费同一事件流，坐标理论上一致。
///
/// 对外接口：PointerFn(x, y, button, pressed)——button 0=左键 1=右键；
/// 点击（pressed）时回调，坐标为屏幕绝对坐标。上层（App）据此做
/// viewport 命中判断与坐标上报（协议层预留接口）。
///
/// RAII：stop() 幂等；fd 与 glib 源一并清理；禁止拷贝。
class EvdevReader {
public:
    using PointerFn =
        std::function<void(std::int32_t x, std::int32_t y,
                           std::int32_t button, bool pressed)>;
    /// 纯移动回调（可选，默认不设）：每次 REL_X/REL_Y 更新坐标后立即
    /// 调用，跟 PointerFn（仅按下沿）完全独立——只是给 CursorOverlay 这
    /// 类"需要跟手"的消费方开的口子，不影响 PointerFn 原有语义/调用
    /// 时机。不需要时留空即可，没有额外开销。
    using MoveFn = std::function<void(std::int32_t x, std::int32_t y)>;

    struct Options {
        std::string basePath = "/dev/input";  ///< evdev 设备目录
        std::int32_t screenW = 1920;          ///< 坐标累积边界（屏幕分辨率）
        std::int32_t screenH = 720;
    };

    EvdevReader() = default;
    ~EvdevReader();
    EvdevReader(const EvdevReader&) = delete;
    EvdevReader& operator=(const EvdevReader&) = delete;

    /// 扫描 basePath 下 event* 设备，挑选具有鼠标能力（REL_X/Y + 左右键）
    /// 的节点，挂 glib 主循环。至少一个设备成功返回 true。onMove 可选。
    bool start(Log& log, const Options& opts, PointerFn onPointer,
               MoveFn onMove = nullptr);
    void stop();

    /// 当前累积坐标（屏幕绝对坐标；起点为屏幕中心）
    std::int32_t pointerX() const { return x_; }
    std::int32_t pointerY() const { return y_; }

private:
    struct Dev {
        int fd = -1;
        guint source = 0;  // glib 源 id
    };

    bool isMouse(int fd) const;
    void onReadable(int fd);
    static gboolean readableThunk(gint fd, GIOCondition cond, gpointer data);

    Log* log_ = nullptr;
    PointerFn onPointer_;
    MoveFn onMove_;
    std::vector<Dev> devs_;
    std::int32_t screenW_ = 0;  ///< 坐标累积边界
    std::int32_t screenH_ = 0;
    std::int32_t x_ = 0;
    std::int32_t y_ = 0;
};

} // namespace aa
