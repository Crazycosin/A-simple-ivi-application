#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>

#include <glib.h>
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>

#include "media/MediaTypes.h"
#include "util/Log.h"

namespace aa {

/// 统一媒体管线（screencast-avsync-design 落地）：
///
///   GstPipeline（固定 GstSystemClock，不随分支增删切换）
///   ├── video_bin（动态挂拆）：v_appsrc → v_queue(无leaky) → decodebin
///   │     → videoconvert(BGRx) → appsink（帧交主线程，绘入应用窗口）
///   └── audio_bin（动态挂拆）：a_appsrc → a_queue → aacparse → avdec_aac
///         → audioconvert/audioresample(S16LE/2ch/48k) → alsasink
///
/// 数据面统一接口（video_audio_pipeline_design：file / packet 同一入口）：
///   pushVideoPacket/pushAudioPacket（任意线程，block 反压不丢包）。
///   文件源经 FilePacketFeeder 拆包喂进同一接口；投屏协议层将来直接调。
///
/// 时间戳（§3.1）：以第一个到达的媒体包建立 base_ts，PTS = capture_ts -
/// base_ts，音视频共用；仅 IDR 恢复流程重置。
///
/// 卡顿恢复（§4.2）：监测 v_queue 水位超阈值 → 丢弃非 IDR 包 → 新 IDR
/// 到达时 flush 解码前 queue 并重置 base_ts。投屏模式由协议层请求 IDR，
/// 文件模式不会触发。
///
/// 音画纠偏（§7）：仅两路并存时运行，小步长只调视频 appsink 的
/// ts-offset（不动音频，避免爆音）。
class MediaPlayer {
public:
    struct Callbacks {
        /// 主线程：有新帧待取（此时应 pullVideoFrame()）
        std::function<void()> onVideoFrame;
        /// 主线程：卡顿已判定、开始等 IDR（上层负责向协议端请求 IDR）
        std::function<void()> onStall;
        /// 主线程：管线级错误（字符串简述）
        std::function<void(const std::string&)> onError;
    };

    struct Options {
        Transport transport = Transport::Usb;
        std::uint64_t queueMaxNsOverride = 0;       // 0 = 用 profile 值
        std::uint64_t stallThresholdNsOverride = 0; // 0 = 用 profile 值
        std::uint32_t driftIntervalSec = 2;         // 纠偏周期；0 = 关
        std::uint32_t stallPollMs = 200;            // 卡顿检测轮询周期
        std::uint32_t stallGiveUpSec = 5;           // 等 IDR 超时放弃（防死等）
        /// 卡顿检测 + IDR 恢复（§4.2）总开关。这是投屏网络卡顿的恢复
        /// 机制：需要"向发送端请求 IDR"的通道配合。文件源没有该通道，
        /// 且追帧期 queue 顶满会误触发（丢包 + base_ts 重置会把音画
        /// 时间线搞乱，实测循环卡死）——文件源必须关。
        bool stallDetect = false;
        /// drm 输出：视频链走 omx use-dmabuf=true → appsink(memory:DMABuf)，
        /// 帧输出为 fd 描述（VideoFrame.isDmabuf），供 DRM plane 直扫。
        /// false = shm 路径（videoconvert BGRx → 内存帧）。
        bool drmOutput = false;
        /// VPU dmabuf 布局 modifier（caps/video-meta 不携带，由配置注入；
        /// h265 wave412 = Semidrive WAVE_32X8_TILE）。0 = LINEAR。
        std::uint64_t drmModifier = 0;
    };

    MediaPlayer(Log& log, Callbacks cbs, Options opts);
    ~MediaPlayer();

    MediaPlayer(const MediaPlayer&) = delete;
    MediaPlayer& operator=(const MediaPlayer&) = delete;

    /// 建管线（系统时钟）并置 PLAYING。失败返回 false。
    bool start();
    /// 停全部会话、置 NULL、释放。会先释放当前帧。
    void stop();

    // ---- 会话（§5 动态分支：音频后到/消失都支持）----
    /// 首个包的 caps 到达时会自动起会话；也可显式预起（无数据无害）。
    void startVideoSession();
    void stopVideoSession();
    void startAudioSession();
    void stopAudioSession();
    bool videoActive() const;
    bool audioActive() const;

    // ---- 数据面（任意线程；线程安全；block=TRUE 反压）----
    /// 首包前设置（文件源：parsebin pad caps；投屏：协议层给出）。
    /// 顺带按需创建会话。
    void setVideoCaps(const GstCaps* caps);
    void setAudioCaps(const GstCaps* caps);
    /// ptsNs/dtsNs 为发送端单调时钟域的捕获时间戳（文件源：demuxer PTS）。
    /// isIdr：IDR/关键帧标记（等待 IDR 恢复时丢弃非 IDR 包）。
    void pushVideoPacket(const std::uint8_t* data, std::size_t size,
                         std::uint64_t ptsNs, std::uint64_t dtsNs,
                         bool hasDts, bool isIdr);
    void pushAudioPacket(const std::uint8_t* data, std::size_t size,
                         std::uint64_t ptsNs);
    /// 流结束（EOS）通知
    void endVideoStream();
    void endAudioStream();

    // ---- 帧输出（主线程）----
    bool pullVideoFrame(VideoFrame& out);
    /// 按 PTS 取读取点时刻（µs 单调时钟）并清理更早条目；0 = 未抓到
    std::int64_t takeReadTimeUs(std::int64_t ptsNs);

private:
    // C 回调 thunk（static + data 转回 this）
    static GstFlowReturn onNewSampleThunk(GstElement* sink, gpointer data);
    static gboolean frameIdleThunk(gpointer data);
    static gboolean busThunk(GstBus* bus, GstMessage* msg, gpointer data);
    static gboolean stallTickThunk(gpointer data);
    static gboolean driftTickThunk(gpointer data);
    static gboolean diagTickThunk(gpointer data);

    gboolean busWatch(GstMessage* msg);
    void onDecPadAdded(GstPad* pad);
    gboolean stallTick();
    gboolean driftTick();
    gboolean diagTick();

    // mu_ 保护：会话创建/拆除（feeder 线程经 setCaps 触发）与 push 并发
    void startVideoSessionLocked();
    void stopVideoSessionLocked();
    void startAudioSessionLocked();
    void stopAudioSessionLocked();
    void releaseFrameLocked();

    Log& log_;
    Callbacks cbs_;
    Options opts_;
    TransportProfile profile_;

    // glib 源 id
    guint stallTimer_ = 0;
    guint driftTimer_ = 0;
    guint diagTimer_ = 0;
    std::uint64_t pushedVideoPkts_ = 0;  // 诊断：已 push 的包数
    std::uint64_t pushedAudioPkts_ = 0;

    mutable std::mutex mu_;
    GstElement* pipeline_ = nullptr;  // 持有
    // video 会话（bin 挂在 pipeline_ 上；元素引用归 bin，此处借用）
    GstElement* vBin_ = nullptr;
    GstElement* vSrc_ = nullptr;
    GstElement* vQueue_ = nullptr;
    GstElement* vConv_ = nullptr;
    GstElement* vSink_ = nullptr;
    std::string vCapsStr_;            // 去重用
    std::string vCapsPending_;        // 首包 caps（决定 parser/解码器）
    // audio 会话
    GstElement* aBin_ = nullptr;
    GstElement* aSrc_ = nullptr;
    GstElement* aQueue_ = nullptr;
    GstElement* aSink_ = nullptr;
    std::string aCapsStr_;

    // 时间基准（§3.1）：首个媒体包（任意一路）确定；IDR 恢复时重置。
    // 用显式标志做哨兵：投屏捕获时钟 ns 不会为 0，但文件源 PTS 从 0
    // 开始，用 baseTs_==0 判断会让文件首包（PTS=0）之后第二包覆盖基准
    std::uint64_t baseTs_ = 0;
    bool baseTsSet_ = false;
    // 卡顿恢复状态（§4.2）
    bool waitingIdr_ = false;
    // 音频链环境性失败（ALSA 权限/占用）标志：置位后本会话周期不再
    // 重建音频 bin（否则 feeder 每包 setCaps 都重建→失败→拆，死循环
    // 刷日志）；stop()/start() 完整重启时重置
    bool audioDead_ = false;
    bool stallArmed_ = false;   // 首帧解码成功后才武装（防启动误判）
    std::int64_t waitIdrStartUs_ = 0;
    std::int64_t stallOverSinceUs_ = 0;  // queue 持续超阈值起点（确认窗口）
    std::uint64_t stallDroppedPkts_ = 0;

    // 当前帧（主线程独占；sample/map 生命周期至下一次 pull）
    GstSample* sample_ = nullptr;
    // drm 路径：上一帧 sample 再保一拍（plane 异步换帧期防 omx pool
    // 复写同一块内存；GEM handle 只保内存不保内容）
    GstSample* prevSample_ = nullptr;
    GstBuffer* mappedBuf_ = nullptr;
    GstMapInfo map_{};
    VideoFrame frame_{};

    // 帧就绪标记：流线程置位，主线程 idle 消费
    std::atomic<bool> framePending_{false};

    // 时延读取点表：push 线程写、主线程读（PTS(已扣 base) → µs）
    // 由 mu_ 保护
    std::map<std::int64_t, std::int64_t> traceRead_;
};

} // namespace aa
