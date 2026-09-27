#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include <glib.h>
#include <gst/gst.h>

#include "util/Log.h"

namespace aa {

/// 文件→媒体包桥（video_audio_pipeline_design 的 file 输入形态）：
/// filesrc ! parsebin（demux+parse）→ 每个输出 pad 挂 fakesink，
/// handoff 回调里把编码包（含 PTS/DTS/delta 标记）经 Hooks 送出——
/// 与投屏协议层的网络包走完全相同的下游接口（MediaPlayer::push*）。
///
/// 循环播放：EOS 时 PTS 偏移累加一整轮时长后 seek 回 0，保证下游
/// live 管线的 PTS 时间线单调连续（避免循环边界帧倾泻）。
class FilePacketFeeder {
public:
    /// 注意：回调在 feeder 的流线程上触发（可阻塞，下游 block 反压）
    struct Hooks {
        /// 视频编码包（data 在回调返回后失效；pts/dts 已含循环偏移）
        std::function<void(const std::uint8_t* data, std::size_t size,
                           std::uint64_t ptsNs, std::uint64_t dtsNs,
                           bool hasDts, bool isIdr, const GstCaps* caps)>
            onVideo;
        /// 音频编码包
        std::function<void(const std::uint8_t* data, std::size_t size,
                           std::uint64_t ptsNs, const GstCaps* caps)>
            onAudio;
        /// 非循环模式整文件 EOS（主线程）
        std::function<void()> onEos;
    };

    FilePacketFeeder(Log& log, std::string uri, Hooks hooks);
    ~FilePacketFeeder();

    FilePacketFeeder(const FilePacketFeeder&) = delete;
    FilePacketFeeder& operator=(const FilePacketFeeder&) = delete;

    void setLoop(bool loop) { loop_ = loop; }

    bool start();
    void stop();

private:
    static void padAddedThunk(GstElement* parsebin, GstPad* pad,
                              gpointer data);
    static void handoffThunk(GstElement* fakesink, GstBuffer* buf, GstPad* pad,
                             gpointer data);
    static gboolean busThunk(GstBus* bus, GstMessage* msg, gpointer data);

    void onPadAdded(GstPad* pad);
    void onHandoff(GstBuffer* buf, GstPad* pad);
    gboolean busWatch(GstMessage* msg);

    Log& log_;
    std::string uri_;
    Hooks hooks_;
    bool loop_ = true;

    GstElement* pipeline_ = nullptr;  // filesrc ! parsebin（+动态 fakesink）
    std::uint64_t ptsOffsetNs_ = 0;   // 循环轮次偏移（保持 PTS 单调）
};

} // namespace aa
