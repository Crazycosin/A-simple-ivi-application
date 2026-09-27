#pragma once

#include <cstdint>
#include <string>

#include <glib.h>

#include "util/Log.h"

namespace aa {

/// 独立小 overlay plane 画鼠标光标（调试期可见性用）。
///
/// 背景（2026-09-18 排查）：DrmView 的视频 plane 是纯不透明覆盖
/// （blend=none），而这块 DPU 没有独立硬件光标 plane——weston 的鼠标是
/// 软件合成进它自己 primary 画面里的，视频 plane 一盖，光标必然被挡住，
/// 跟 zpos 数值无关，是结构性问题。这个类不改视频链路，另开一路独立
/// plane 专门画一个固定箭头位图，zpos 压在视频之上（视频用 2，这里用
/// 3——当年设计 zpos 时就是照着"顶层 RGB 层"预留的这个位置）。
///
/// 与 DrmView 完全独立、互不感知、互不依赖：自己开一路 /dev/dri fd，
/// 自己挑一个视频 plane 之外的空闲 ARGB8888 overlay plane。调用方只需
/// 告诉它 DrmView 正在用的 crtcId 与 planeId（避免选中同一块 plane——
/// create() 时视频可能还没提交首帧，FB_ID 尚为 0，光靠占用检测分不出
/// 来，必须显式跳过）。
///
/// 生命周期三步：create（画一次箭头位图、setplane 到初始位置）→
/// moveTo（每次鼠标移动只挪 plane 目标矩形，不重绘内容）→ destroy（关
/// plane、还 dumb buffer/GEM）。失败（没有空闲 ARGB overlay plane 等）
/// 只记日志、返回 false，不影响视频路径——光标是锦上添花，不是关键路径。
///
/// 触摸驱动落地后这层没有存在意义（触摸没有光标概念）：删除本文件
/// + AndroidAutoApp.h/.cpp 里标了 "CursorOverlay" 的几处调用即可完整
/// 移除，不影响 DrmView/MediaPlayer/EvdevReader 的其他逻辑。
/// EvdevReader 多出的 MoveFn 回调若无人使用可一并留空，不必强制清理。
///
/// RAII：destroy() 幂等；禁止拷贝。
class CursorOverlay {
public:
    CursorOverlay() = default;
    ~CursorOverlay();
    CursorOverlay(const CursorOverlay&) = delete;
    CursorOverlay& operator=(const CursorOverlay&) = delete;

    /// device：DRM 设备节点（与 DrmView 用同一个，独立 open 一路 fd，
    /// 本平台已验证多路非 master fd 可共存 setplane）；crtcId：
    /// DrmView::crtcId()；avoidPlaneId：DrmView::planeId()。
    bool create(Log& log, const std::string& device, std::uint32_t crtcId,
                std::uint32_t avoidPlaneId);
    void destroy();

    /// 挪光标到屏幕坐标（箭头图标左上角为热点，即坐标本身就是尖端位置）。
    /// 内部按显示帧率节流（见 .cpp 注释）：调用频率可以远高于此，超额
    /// 部分会被合并，只留最新目标坐标，不丢最终位置。
    void moveTo(std::int32_t screenX, std::int32_t screenY);

    bool valid() const { return devFd_ >= 0 && planeId_ != 0; }

private:
    bool pickPlane(std::uint32_t avoidPlaneId);
    bool setPlaneProp(const char* name, std::uint64_t value);
    void drawArrow();  // cairo 画箭头进 mmap 出来的 dumb buffer 内存
    void commit(std::int32_t x, std::int32_t y);  // 真正发 drmModeSetPlane
    static gboolean flushThunk(gpointer data);
    void onFlushTimer();

    Log* log_ = nullptr;
    int devFd_ = -1;
    std::uint32_t crtcId_ = 0;
    std::uint32_t planeId_ = 0;
    std::uint32_t fbId_ = 0;
    std::uint32_t gemHandle_ = 0;
    std::uint32_t pitch_ = 0;
    void* mapped_ = nullptr;
    std::uint64_t mappedSize_ = 0;
    std::int32_t w_ = 0;
    std::int32_t h_ = 0;
    std::int32_t curX_ = 0;
    std::int32_t curY_ = 0;

    // 节流合并状态（见 moveTo 注释）
    guint flushSource_ = 0;      ///< 待执行的一次性 flush 定时器；0=未挂起
    std::int32_t pendingX_ = 0;  ///< 节流窗口内的最新目标坐标
    std::int32_t pendingY_ = 0;
    bool hasPending_ = false;
    gint64 lastCommitUs_ = 0;    ///< 上次真正提交 setplane 的时刻
};

} // namespace aa
