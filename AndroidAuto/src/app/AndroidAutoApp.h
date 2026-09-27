#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <glib.h>
#include <wayland-client.h>

#include "app/InputController.h"
#include "input/EvdevReader.h"
#include "media/MediaTypes.h"
#include "model/AppConfig.h"
#include "model/AppModel.h"
#include "util/CursorOverlay.h"
#include "util/Log.h"
#include "view/AppView.h"
#include "view/DrmView.h"
#include "wayland/ShmBuffer.h"
#include "wayland/WlClient.h"

namespace aa {

class MediaPlayer;
class FilePacketFeeder;

/// 组合根：装配 MVC，运行 glib 主循环。
///
/// 主循环事件源（全部汇到主线程，唯一允许碰 wayland 的线程）：
///   wayland socket fd → dispatch（configure/输入/buffer release）
///   MediaPlayer 帧就绪 idle → pullVideoFrame → repaint
///   MediaPlayer 总线 watch / 卡顿检测 / 音画纠偏（内部定时器）
///   FilePacketFeeder 总线 watch（循环 seek / EOS）
///
/// 媒体架构见 media/MediaPlayer.h（screencast-avsync-design 落地）：
/// 音视频共用一条固定系统时钟的管线，文件源经 FilePacketFeeder
/// 拆包喂进媒体包接口（与投屏协议层同一路径）。
///
/// 时延统计链路：
///   读取点（push 包，MediaPlayer 内）→ 拉出点（pullVideoFrame）
///   → 提交点（attach/commit）→ 上屏点（wl_surface_frame done），
///   [latency] 逐段打印，退出打总结。
class AndroidAutoApp {
public:
    /// exeDir: /proc/self/exe 所在目录（ini 按此搜索）
    /// 返回 0 正常退出；2 环境错误
    static int run(const std::string& exeDir);

private:
    explicit AndroidAutoApp(Config cfg);
    bool setup(const std::string& iniPath, bool hasIni, std::size_t cfgErrs);
    void mainLoop();
    void StartUsbMonitoring();
    void StopUsbMonitoring();
    void PrintUsbStatus();
    /// 停 feeder/媒体管线 + 时延总结 + 布局恢复（mainLoop 返回后调）
    void teardown();
    void repaint();
    void maybeRepaint();

    /// MediaPlayer 帧就绪通知（glib idle，主线程）
    void onVideoFrame();
    /// 时延·上屏点（wl_surface_frame done 回调）
    void reportLatency();
    void logLatencySummary();

    /// ---- DRM 直扫分支（render_path=drm，混合路线 C）----
    /// dmabuf 帧经 DrmView 提交 plane（不走 cairo/wl_shm）
    bool submitDrmFrame(const VideoFrame& f);
    /// evdev 视频区坐标上报（协议层预留接口：现打日志，将来接投屏
    /// 协议的触摸/按键注入通道）
    void onEvdevPointer(std::int32_t x, std::int32_t y,
                        std::int32_t button, bool pressed);

    ShmBuffer& acquireFreeBuffer();

    // ---- glib / wayland C 回调 thunk ----
    static gboolean onWlReadableThunk(gint fd, GIOCondition cond,
                                      gpointer data);
    static gboolean onQuitCheckThunk(gpointer data);
    static gboolean onSignalThunk(gpointer data);
    static void onFrameDoneThunk(void* data, wl_callback* cb,
                                 std::uint32_t time);
    static const wl_callback_listener kFrameListener;

    // 声明顺序即初始化顺序（InputController 引用前三个成员）
    const Config cfg_;
    AppModel model_;
    Log log_;
    InputController inputCtrl_;
    std::unique_ptr<AppView> view_;
    WlClient wl_;
    ShmBuffer buffers_[2];
    std::uint8_t currentIdx_ = 0;

    // 媒体（统一管线 + 文件拆包桥）
    std::unique_ptr<MediaPlayer> media_;
    std::unique_ptr<FilePacketFeeder> videoFeeder_;
    std::unique_ptr<FilePacketFeeder> audioFeeder_;
    VideoFrame currentFrame_{};   ///< 当前帧（数据有效至下一次 pull）

    // DRM 直扫分支（render_path=drm）
    DrmView drmView_;            ///< plane 直扫提交（无效则回退 shm 绘制）
    EvdevReader evdev_;          ///< 视频区鼠标坐标（weston 看不到 plane 区域）
    CursorOverlay cursor_;       ///< 独立 plane 画鼠标光标（调试用，可整体删除，见类注释）
    bool drmActive_ = false;     ///< DrmView create 成功且本帧走 plane

    GMainLoop* loop_ = nullptr;
    gint64 tStartUs_ = 0;         ///< 启动基准（首帧上屏计时用）
    bool layoutSwitched_ = false; ///< 本次运行是否真的切换过布局（退出恢复用）

    // 时延统计（全部主线程访问）
    struct PendingLatency {
        bool valid = false;
        std::int64_t ptsNs = -1;
        std::int64_t tReadUs = 0;   ///< 读取点（push 包时刻；0 = 未抓到）
        std::int64_t tPullUs = 0;   ///< 拉出点（pullVideoFrame）
        std::int64_t tCommitUs = 0; ///< 提交点（attach/commit）
    } pendingLat_;
    bool frameCbPending_ = false;  ///< 一次只挂一个 frame callback 防串帧
    bool firstScreenDone_ = false;
    std::uint64_t latFrames_ = 0;
    struct LatStats {
        std::uint64_t n = 0;
        double sum = 0, min = 1e18, max = 0;
        void add(double ms) {
            ++n;
            sum += ms;
            if (ms < min) min = ms;
            if (ms > max) max = ms;
        }
    };
    LatStats stReadPull_, stPullCommit_, stCommitScreen_, stTotal_;
};

} // namespace aa
