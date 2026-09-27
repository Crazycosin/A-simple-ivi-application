#include "media/MediaPlayer.h"

#include <cstring>
#include <utility>

#include <drm_fourcc.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/video/video.h>

namespace aa {

namespace {

/// 队列（§4.1）：无 leaky、按时间限量；顶满后上游（appsrc push）阻塞
GstElement* makeQueue(const char* name, std::uint64_t maxTimeNs) {
    GstElement* q = gst_element_factory_make("queue", name);
    g_object_set(q, "max-size-time", maxTimeNs, "max-size-buffers", 0,
                 "max-size-bytes", 0, "leaky", 0, nullptr);
    return q;
}

/// appsrc（§3.2）：外部打 PTS、block 反压。
/// is-live=FALSE：live 源在 PAUSED 不推数据，而动态挂载的 bin 需 sink
/// preroll（收到首帧）才能到 PLAYING——live 语义在这里死锁（实测整链
/// 卡死不出帧）。数据到达节奏由 feeder 的 fakesink sync=TRUE 1x 重放
/// 控制，不需要 appsrc 自己的 live 时钟语义。
GstElement* makeAppSrc(const char* name) {
    GstElement* src = gst_element_factory_make("appsrc", name);
    g_object_set(src, "format", GST_FORMAT_TIME, "is-live", FALSE,
                 "do-timestamp", FALSE, "stream-type",
                 GST_APP_STREAM_TYPE_STREAM, "block", TRUE, "max-bytes",
                 static_cast<guint64>(32u << 20), "min-latency",
                 static_cast<gint64>(0), nullptr);
    return src;
}

std::string capsToString(const GstCaps* caps) {
    if (caps == nullptr) {
        return {};
    }
    gchar* s = gst_caps_to_string(caps);
    std::string r = s != nullptr ? s : "";
    g_free(s);
    return r;
}

/// 首包 caps → 编码格式名（"h264"/"h265"；其他返回空）。
/// 只看 media type，不认具体容器细节（stream-format/alignment 由
/// parser 适配）。
std::string videoFormatFromCaps(const std::string& capsStr) {
    if (capsStr.find("video/x-h265") != std::string::npos) {
        return "h265";
    }
    if (capsStr.find("video/x-h264") != std::string::npos) {
        return "h264";
    }
    return {};
}

/// GST 像素格式 → DRM fourcc（drm 路径用；仅列 VPU 实际输出格式，
/// 其他格式记日志走不通——避免错格式直扫出花屏）
std::uint32_t gstFormatToDrmFourcc(GstVideoFormat fmt) {
    switch (fmt) {
    case GST_VIDEO_FORMAT_NV12: return DRM_FORMAT_NV12;
    case GST_VIDEO_FORMAT_NV21: return DRM_FORMAT_NV21;
    case GST_VIDEO_FORMAT_NV16: return DRM_FORMAT_NV16;
    case GST_VIDEO_FORMAT_NV24: return DRM_FORMAT_NV24;
    case GST_VIDEO_FORMAT_BGRA: return DRM_FORMAT_ARGB8888;
    case GST_VIDEO_FORMAT_BGRx: return DRM_FORMAT_XRGB8888;
    default: return 0;
    }
}

} // namespace

MediaPlayer::MediaPlayer(Log& log, Callbacks cbs, Options opts)
    : log_(log), cbs_(std::move(cbs)), opts_(opts) {
    profile_ = profileFor(opts_.transport);
    if (opts_.queueMaxNsOverride != 0) {
        profile_.preDecodeQueueMaxNs = opts_.queueMaxNsOverride;
    }
    if (opts_.stallThresholdNsOverride != 0) {
        profile_.stallThresholdNs = opts_.stallThresholdNsOverride;
    }
}

MediaPlayer::~MediaPlayer() {
    stop();
}

// ---------------------------------------------------------------------------
// 管线生命周期
// ---------------------------------------------------------------------------

bool MediaPlayer::start() {
    std::lock_guard<std::mutex> lk(mu_);
    if (pipeline_ != nullptr) {
        return true;
    }
    pipeline_ = gst_pipeline_new("media-pipeline");
    if (pipeline_ == nullptr) {
        log_.write("[media] pipeline create failed");
        return false;
    }
    // §3.3 固定系统时钟：音频 sink 会提供 audio clock，放任自动选择的话
    // audio_bin 挂拆时管线会切时钟基准导致卡顿；显式钉死
    gst_pipeline_use_clock(GST_PIPELINE(pipeline_), gst_system_clock_obtain());

    GstBus* bus = gst_element_get_bus(pipeline_);
    gst_bus_add_watch(bus, &MediaPlayer::busThunk, this);
    gst_object_unref(bus);

    // live 管线：直接 PLAYING，等数据流入（分支后挂用 sync_state_with_parent）
    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE) {
        log_.write("[media] set_state(PLAYING) failed");
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
        return false;
    }

    stallTimer_ = opts_.stallDetect
                      ? g_timeout_add(opts_.stallPollMs,
                                      &MediaPlayer::stallTickThunk, this)
                      : 0;
    diagTimer_ = g_timeout_add_seconds(2, &MediaPlayer::diagTickThunk, this);
    if (opts_.driftIntervalSec > 0) {
        driftTimer_ = g_timeout_add_seconds(opts_.driftIntervalSec,
                                            &MediaPlayer::driftTickThunk, this);
    }
    log_.write("[media] pipeline started (clock=system, queue_max=%luns, "
               "stall_thr=%luns)",
               static_cast<unsigned long>(profile_.preDecodeQueueMaxNs),
               static_cast<unsigned long>(profile_.stallThresholdNs));
    return true;
}

void MediaPlayer::stop() {
    guint stallT = 0;
    guint driftT = 0;
    guint diagT = 0;
    {
        std::lock_guard<std::mutex> lk(mu_);
        stallT = stallTimer_;
        driftT = driftTimer_;
        diagT = diagTimer_;
        stallTimer_ = 0;
        driftTimer_ = 0;
        diagTimer_ = 0;
    }
    if (stallT != 0) {
        g_source_remove(stallT);
    }
    if (driftT != 0) {
        g_source_remove(driftT);
    }
    if (diagT != 0) {
        g_source_remove(diagT);
    }

    std::lock_guard<std::mutex> lk(mu_);
    releaseFrameLocked();
    stopVideoSessionLocked();
    stopAudioSessionLocked();
    audioDead_ = false;  // 完整重启允许音频再试（环境可能已修复）
    if (pipeline_ != nullptr) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
        log_.write("[media] pipeline stopped");
    }
}

// ---------------------------------------------------------------------------
// 会话（§5 动态分支）
// ---------------------------------------------------------------------------

void MediaPlayer::startVideoSession() {
    std::lock_guard<std::mutex> lk(mu_);
    startVideoSessionLocked();
}

void MediaPlayer::startVideoSessionLocked() {
    if (pipeline_ == nullptr || vBin_ != nullptr) {
        return;
    }
    // 显式链路（设计文档 §2）：appsrc → queue → parse → dec → convert(BGRx)
    // → appsink。不用 decodebin：与 appsrc 组合时其协商挂起（实测卡
    // PREROLLING 不出帧），且文档本来就要求固定链路。parser/解码器按
    // 首包 caps 的格式选定（h264/h265），硬解 omx 优先、创建失败落软解。
    const std::string fmt = videoFormatFromCaps(vCapsPending_);
    if (fmt.empty()) {
        log_.write("[media:video] cannot determine codec from caps, "
                   "session not started");
        return;
    }

    vBin_ = gst_bin_new("video_bin");
    vSrc_ = makeAppSrc("v_appsrc");
    vQueue_ = makeQueue("v_queue", profile_.preDecodeQueueMaxNs);
    GstElement* parse = nullptr;
    GstElement* dec = nullptr;
    std::string parserName;
    std::string decName;
    if (fmt == "h265") {
        parserName = "h265parse";
        decName = "omxh265dec";
    } else if (fmt == "h264") {
        parserName = "h264parse";
        decName = "omxh264dec";
    }
    parse = gst_element_factory_make(parserName.c_str(), "v_parse");
    dec = gst_element_factory_make(decName.c_str(), "v_dec");
    if (dec == nullptr) {
        // 硬解不可用 → libav 软解兜底（avdec 来自 /opt/x9hp）
        decName = "avdec_" + fmt;
        dec = gst_element_factory_make(decName.c_str(), "v_dec");
        log_.write("[media:video] hw decoder unavailable, fallback %s",
                   decName.c_str());
    }
    // drm 路径：开 omx dmabuf 输出（实测 315fps 零拷贝路径；默认 false，
    // 不开则解码输出走 TILED→LINEAR CPU 拷贝）。软解无此属性（跳过，
    // 其输出为系统内存，上层按 shm 帧回退处理）。
    if (opts_.drmOutput &&
        g_object_class_find_property(G_OBJECT_GET_CLASS(dec), "use-dmabuf") !=
            nullptr) {
        g_object_set(dec, "use-dmabuf", TRUE, nullptr);
        log_.write("[media:video] dmabuf output enabled (use-dmabuf=true)");
    }
    if (opts_.drmOutput) {
        // drm 路径链路：appsrc → queue → parse → dec → appsink。
        // 不插 videoconvert/capsfilter：任何内存格式转换都会把 dmabuf
        // 拉回系统内存（拷贝路径）；NV12 直扫由 DRM plane 完成。
        vSink_ = gst_element_factory_make("appsink", "v_sink");
        if (vBin_ == nullptr || vSrc_ == nullptr || vQueue_ == nullptr ||
            parse == nullptr || dec == nullptr || vSink_ == nullptr) {
            log_.write("[media:video] element create failed (%s/%s)",
                       parserName.c_str(), decName.c_str());
            gst_object_unref(vBin_);
            vBin_ = vSrc_ = vQueue_ = vConv_ = vSink_ = nullptr;
            return;
        }
        // wait-on-eos=FALSE：appsink 默认收到 EOS 事件时会在解码器的
        // streaming 线程里阻塞等 app 把已入队的样本全部 pull 完（见
        // gstappsink.c gst_app_sink_event() 的 GST_EVENT_EOS 分支），
        // 目的是不让 EOS 抢在样本被消费前发出。实测非循环播放到片尾
        // 时，紧接着的 stopVideoSessionLocked()→drmModeSetPlane(NULL)
        // 状态切换会卡死在这个等待上（主线程 futex_wait 卡死，SIGTERM
        // 不起作用只能 SIGKILL）。本应用靠 new-sample 信号异步 pull，
        // 不需要这条"EOS 前必须排空"的保证，关掉换取干净退出。
        g_object_set(vSink_, "emit-signals", TRUE, "sync", FALSE,
                     "max-buffers", 2, "drop", TRUE, "wait-on-eos", FALSE,
                     nullptr);
        gst_bin_add_many(GST_BIN(vBin_), vSrc_, vQueue_, parse, dec, vSink_,
                         nullptr);
        if (!gst_element_link_many(vSrc_, vQueue_, parse, dec, vSink_,
                                   nullptr)) {
            log_.write("[media:video] bin link failed (drm path)");
            gst_object_unref(vBin_);
            vBin_ = vSrc_ = vQueue_ = vConv_ = vSink_ = nullptr;
            return;
        }
    } else {
        vConv_ = gst_element_factory_make("videoconvert", "v_conv");
        GstElement* cf = gst_element_factory_make("capsfilter", "v_bgrx");
        vSink_ = gst_element_factory_make("appsink", "v_sink");
        if (vBin_ == nullptr || vSrc_ == nullptr || vQueue_ == nullptr ||
            parse == nullptr || dec == nullptr || vConv_ == nullptr ||
            cf == nullptr || vSink_ == nullptr) {
            log_.write("[media:video] element create failed (%s/%s)",
                       parserName.c_str(), decName.c_str());
            gst_object_unref(vBin_);
            vBin_ = vSrc_ = vQueue_ = vConv_ = vSink_ = nullptr;
            return;
        }
        // weston pixman 软合成经 wl_shm 要求 BGRx（硬解输出不显式转换时
        // 协商不到 wl_shm 能吃的格式）
        GstCaps* bgrx = gst_caps_new_simple("video/x-raw", "format",
                                            G_TYPE_STRING, "BGRx", nullptr);
        g_object_set(cf, "caps", bgrx, nullptr);
        gst_caps_unref(bgrx);
        // appsink 不做时钟同步：渲染节奏由数据到达节奏决定（feeder 1x 重放
        // /投屏包实时到达），主循环 idle 即拉即绘；音频侧 alsasink sync=TRUE
        // 按 PTS 播放提供基准，音画偏差由 ts-offset 纠偏（§7）。
        // （sync=TRUE 实测会让 appsink 预卷等待时钟，整条链路卡死不出帧）
        // wait-on-eos=FALSE：同 drm 路径分支的理由（见上方 drm 分支
        // 注释）——非循环播放到片尾时避免 EOS 处理卡在等 app 排空队列。
        g_object_set(vSink_, "emit-signals", TRUE, "sync", FALSE,
                     "max-buffers", 2, "drop", TRUE, "wait-on-eos", FALSE,
                     nullptr);

        gst_bin_add_many(GST_BIN(vBin_), vSrc_, vQueue_, parse, dec, vConv_,
                         cf, vSink_, nullptr);
        if (!gst_element_link_many(vSrc_, vQueue_, parse, dec, vConv_, cf,
                                   vSink_, nullptr)) {
            log_.write("[media:video] bin link failed");
            gst_object_unref(vBin_);
            vBin_ = vSrc_ = vQueue_ = vConv_ = vSink_ = nullptr;
            return;
        }
    }
    g_signal_connect(vSink_, "new-sample",
                     G_CALLBACK(&MediaPlayer::onNewSampleThunk), this);

    gst_bin_add(GST_BIN(pipeline_), vBin_);
    gst_element_sync_state_with_parent(vBin_);
    log_.write("[media:video] session started");
}

void MediaPlayer::stopVideoSession() {
    std::lock_guard<std::mutex> lk(mu_);
    stopVideoSessionLocked();
}

void MediaPlayer::stopVideoSessionLocked() {
    if (vBin_ != nullptr) {
        gst_element_set_state(vBin_, GST_STATE_NULL);
        gst_bin_remove(GST_BIN(pipeline_), vBin_);
        vBin_ = vSrc_ = vQueue_ = vConv_ = vSink_ = nullptr;
        vCapsStr_.clear();
        vCapsPending_.clear();
        stallArmed_ = false;  // 会话重建后等新首帧再武装
        waitingIdr_ = false;
        stallOverSinceUs_ = 0;
        releaseFrameLocked();
        log_.write("[media:video] session stopped");
    }
}

void MediaPlayer::startAudioSession() {
    std::lock_guard<std::mutex> lk(mu_);
    startAudioSessionLocked();
}

void MediaPlayer::startAudioSessionLocked() {
    if (pipeline_ == nullptr || aBin_ != nullptr || audioDead_) {
        return;
    }
    aBin_ = gst_bin_new("audio_bin");
    aSrc_ = makeAppSrc("a_appsrc");
    aQueue_ = makeQueue("a_queue", profile_.preDecodeQueueMaxNs);
    // 文档固定链路：aacparse → avdec_aac（软解，来自 /opt/x9hp libgstlibav）
    GstElement* parse = gst_element_factory_make("aacparse", "a_aacparse");
    GstElement* dec = gst_element_factory_make("avdec_aac", "a_avdec");
    GstElement* conv = gst_element_factory_make("audioconvert", "a_conv");
    GstElement* resample = gst_element_factory_make("audioresample", "a_res");
    GstElement* cf = gst_element_factory_make("capsfilter", "a_s16");
    aSink_ = gst_element_factory_make("alsasink", "a_alsa");
    if (aBin_ == nullptr || aSrc_ == nullptr || aQueue_ == nullptr ||
        parse == nullptr || dec == nullptr || conv == nullptr ||
        resample == nullptr || cf == nullptr || aSink_ == nullptr) {
        log_.write("[media:audio] element create failed");
        gst_object_unref(aBin_);
        aBin_ = aSrc_ = aQueue_ = aSink_ = nullptr;
        return;
    }
    // 板级要求：TLV320AIC23 唯一验证格式（asound.conf 已把 default 钉到 card1）
    GstCaps* caps = gst_caps_new_simple(
        "audio/x-raw", "format", G_TYPE_STRING, "S16LE", "channels", G_TYPE_INT,
        2, "rate", G_TYPE_INT, 48000, nullptr);
    g_object_set(cf, "caps", caps, nullptr);
    gst_caps_unref(caps);
    g_object_set(aSink_, "sync", TRUE, nullptr);

    // ALSA 可用性预探测（NULL→READY 即 open 设备）：权限/被占等环境性
    // 失败在挂 bin 前发现，直接不建音频会话。错误态一旦进入管线会拖死
    // 视频链 preroll（实测 v_queue 顶满无帧，事后拆除也救不回）。
    if (gst_element_set_state(aSink_, GST_STATE_READY) ==
        GST_STATE_CHANGE_FAILURE) {
        log_.write("[media:audio] sink open failed (ALSA unavailable), "
                   "audio session skipped to protect video");
        gst_element_set_state(aSink_, GST_STATE_NULL);
        gst_object_unref(aSink_);
        gst_object_unref(cf);
        gst_object_unref(resample);
        gst_object_unref(conv);
        gst_object_unref(dec);
        gst_object_unref(parse);
        gst_object_unref(aQueue_);
        gst_object_unref(aSrc_);
        gst_object_unref(aBin_);
        aBin_ = aSrc_ = aQueue_ = aSink_ = nullptr;
        audioDead_ = true;
        return;
    }
    gst_element_set_state(aSink_, GST_STATE_NULL);

    gst_bin_add_many(GST_BIN(aBin_), aSrc_, aQueue_, parse, dec, conv, resample,
                     cf, aSink_, nullptr);
    if (!gst_element_link_many(aSrc_, aQueue_, parse, dec, conv, resample, cf,
                               aSink_, nullptr)) {
        log_.write("[media:audio] bin link failed");
        gst_object_unref(aBin_);
        aBin_ = aSrc_ = aQueue_ = aSink_ = nullptr;
        return;
    }

    gst_bin_add(GST_BIN(pipeline_), aBin_);
    gst_element_sync_state_with_parent(aBin_);
    log_.write("[media:audio] session started");
}

void MediaPlayer::stopAudioSession() {
    std::lock_guard<std::mutex> lk(mu_);
    stopAudioSessionLocked();
}

void MediaPlayer::stopAudioSessionLocked() {
    if (aBin_ != nullptr) {
        gst_element_set_state(aBin_, GST_STATE_NULL);
        gst_bin_remove(GST_BIN(pipeline_), aBin_);
        aBin_ = aSrc_ = aQueue_ = aSink_ = nullptr;
        aCapsStr_.clear();
        log_.write("[media:audio] session stopped");
    }
}

bool MediaPlayer::videoActive() const {
    std::lock_guard<std::mutex> lk(mu_);
    return vBin_ != nullptr;
}

bool MediaPlayer::audioActive() const {
    std::lock_guard<std::mutex> lk(mu_);
    return aBin_ != nullptr;
}

// ---------------------------------------------------------------------------
// 数据面
// ---------------------------------------------------------------------------

void MediaPlayer::setVideoCaps(const GstCaps* caps) {
    std::lock_guard<std::mutex> lk(mu_);
    if (caps == nullptr || pipeline_ == nullptr) {
        return;
    }
    const std::string s = capsToString(caps);
    if (s == vCapsStr_ && vSrc_ != nullptr) {
        return;  // 去重（feeder 每包都带 caps）
    }
    vCapsStr_ = s;
    vCapsPending_ = s;  // 首包 caps 决定 parser/解码器（显式链路）
    if (vSrc_ == nullptr) {
        startVideoSessionLocked();
    }
    if (vSrc_ != nullptr) {
        GstCaps* c = gst_caps_copy(caps);
        g_object_set(vSrc_, "caps", c, nullptr);
        gst_caps_unref(c);
    }
}

void MediaPlayer::setAudioCaps(const GstCaps* caps) {
    std::lock_guard<std::mutex> lk(mu_);
    if (caps == nullptr || pipeline_ == nullptr) {
        return;
    }
    const std::string s = capsToString(caps);
    if (s == aCapsStr_ && aSrc_ != nullptr) {
        return;
    }
    startAudioSessionLocked();
    if (aSrc_ == nullptr) {
        return;
    }
    GstCaps* c = gst_caps_copy(caps);
    g_object_set(aSrc_, "caps", c, nullptr);
    gst_caps_unref(c);
    aCapsStr_ = s;
}

void MediaPlayer::pushVideoPacket(const std::uint8_t* data, std::size_t size,
                                  std::uint64_t ptsNs, std::uint64_t dtsNs,
                                  bool hasDts, bool isIdr) {
    GstElement* src = nullptr;
    GstBuffer* buf = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (vSrc_ == nullptr) {
            return;  // 无视频会话（未 setCaps / 已停）
        }
        if (waitingIdr_) {
            // §4.2：新 IDR 到来前的旧 GOP 数据整体丢弃
            if (!isIdr) {
                ++stallDroppedPkts_;
                return;
            }
            gst_element_send_event(vQueue_, gst_event_new_flush_start());
            gst_element_send_event(vQueue_, gst_event_new_flush_stop(TRUE));
            baseTs_ = 0;  // 重新建立时间基准
            baseTsSet_ = false;
            waitingIdr_ = false;
            log_.write("[media] IDR recovery: queue flushed, base_ts reset "
                       "(dropped %llu pkts)",
                       static_cast<unsigned long long>(stallDroppedPkts_));
            stallDroppedPkts_ = 0;
        }
        if (!baseTsSet_) {
            baseTs_ = ptsNs;  // §3.1：首个到达的媒体包定基准
            baseTsSet_ = true;
        }
        const std::uint64_t pts = ptsNs >= baseTs_ ? ptsNs - baseTs_ : 0;
        buf = gst_buffer_new_allocate(nullptr, size, nullptr);
        gst_buffer_fill(buf, 0, data, size);
        GST_BUFFER_PTS(buf) = pts;
        if (hasDts) {
            GST_BUFFER_DTS(buf) = dtsNs >= baseTs_ ? dtsNs - baseTs_ : 0;
        }
        // 时延·读取点（push 时刻 ≈ 文件源 demuxer 读出 / 投屏包到达）
        traceRead_[static_cast<std::int64_t>(pts)] = g_get_monotonic_time();
        if (traceRead_.size() > 256) {
            traceRead_.clear();  // 循环/丢包清理兜底（PTS 回头时按序清会误删）
        }
        ++pushedVideoPkts_;
        src = GST_ELEMENT(vSrc_);
        gst_object_ref(src);
    }
    // block=TRUE：解码前 queue 顶满时阻塞在此（反压不丢包，§4.1）。
    // push 消耗 buf 的所有权（含失败路径）
    gst_app_src_push_buffer(GST_APP_SRC(src), buf);
    gst_object_unref(src);
}

void MediaPlayer::pushAudioPacket(const std::uint8_t* data, std::size_t size,
                                  std::uint64_t ptsNs) {
    GstElement* src = nullptr;
    GstBuffer* buf = nullptr;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (aSrc_ == nullptr) {
            return;
        }
        if (!baseTsSet_) {
            baseTs_ = ptsNs;  // 音频包可能先于视频到达（§3.1）
            baseTsSet_ = true;
        }
        buf = gst_buffer_new_allocate(nullptr, size, nullptr);
        gst_buffer_fill(buf, 0, data, size);
        GST_BUFFER_PTS(buf) = ptsNs >= baseTs_ ? ptsNs - baseTs_ : 0;
        ++pushedAudioPkts_;
        src = GST_ELEMENT(aSrc_);
        gst_object_ref(src);
    }
    gst_app_src_push_buffer(GST_APP_SRC(src), buf);
    gst_object_unref(src);
}

void MediaPlayer::endVideoStream() {
    std::lock_guard<std::mutex> lk(mu_);
    if (vSrc_ != nullptr) {
        GstElement* src = GST_ELEMENT(vSrc_);
        gst_object_ref(src);
        gst_app_src_end_of_stream(GST_APP_SRC(src));
        gst_object_unref(src);
    }
}

void MediaPlayer::endAudioStream() {
    std::lock_guard<std::mutex> lk(mu_);
    if (aSrc_ != nullptr) {
        GstElement* src = GST_ELEMENT(aSrc_);
        gst_object_ref(src);
        gst_app_src_end_of_stream(GST_APP_SRC(src));
        gst_object_unref(src);
    }
}

// ---------------------------------------------------------------------------
// 帧路径：解码流线程 new-sample → 原子标记 + idle → 主线程
// ---------------------------------------------------------------------------

GstFlowReturn MediaPlayer::onNewSampleThunk(GstElement* /*sink*/,
                                            gpointer data) {
    auto* self = static_cast<MediaPlayer*>(data);
    // new-sample 在 GStreamer 流线程上触发，帧的消费（pull/map/渲染）
    // 只能在主线程做：这里只做原子去重标记 + g_idle_add（线程安全）
    if (!self->framePending_.exchange(true)) {
        g_idle_add(&MediaPlayer::frameIdleThunk, self);
    }
    return GST_FLOW_OK;
}

gboolean MediaPlayer::frameIdleThunk(gpointer data) {
    auto* self = static_cast<MediaPlayer*>(data);
    self->framePending_ = false;
    if (self->cbs_.onVideoFrame) {
        self->cbs_.onVideoFrame();
    }
    return G_SOURCE_REMOVE;  // 一次性源；有新帧流线程会再投
}

// ---------------------------------------------------------------------------
// 总线
// ---------------------------------------------------------------------------

gboolean MediaPlayer::busThunk(GstBus* /*bus*/, GstMessage* msg,
                               gpointer data) {
    auto* self = static_cast<MediaPlayer*>(data);
    return self->busWatch(msg) ? TRUE : FALSE;
}

gboolean MediaPlayer::busWatch(GstMessage* msg) {
    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
        GError* err = nullptr;
        gchar* dbg = nullptr;
        gst_message_parse_error(msg, &err, &dbg);
        const std::string what =
            err != nullptr ? err->message : "?";
        log_.write("[media] error: %s (%s)", what.c_str(),
                   dbg != nullptr ? dbg : "-");
        g_clear_error(&err);
        g_free(dbg);
        // 音频链错误（ALSA 权限/被占等环境性失败）不能拖死视频：错误态
        // 的 audio bin 挂在管线上会让整条 preroll 卡住（实测 v_queue
        // 顶满无帧）。拆掉音频会话，视频链独立完成 preroll 继续播放。
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (aBin_ != nullptr &&
                GST_MESSAGE_SRC(msg) != nullptr &&
                gst_object_has_as_ancestor(GST_MESSAGE_SRC(msg),
                                           GST_OBJECT(aBin_))) {
                log_.write("[media:audio] sink error in audio bin, dropping "
                           "audio session to keep video alive");
                audioDead_ = true;
                stopAudioSessionLocked();
            }
            // 视频链致命错误（如 VPU SEQ_INIT 失败 "OpenMAX component in
            // error state Stream corrupt"，实测偶发于个别启动时刻）：错误
            // 态的 dec/appsrc 元素仍挂在管线里，appsink 永远收不到样本，
            // 但 feeder 侧完全不知情——会照常按文件时长每轮 EOS 重建自己
            // 那半条链路、继续往 v_queue 里推包，packet 只会在已经死掉的
            // 队列里堆积（实测 v_queue 卡满不再变化），永久不出画面且
            // 无任何用户可见反馈。这里立刻拆掉视频 bin 并清空
            // vCapsStr_/vCapsPending_；下一次 feeder 循环（本来就会,
            // ~文件时长一个周期，见 FilePacketFeeder 的 loop 机制）调用
            // setVideoCaps() 时 vSrc_==nullptr，会被 setVideoCaps 自然
            // 触发 startVideoSessionLocked() 重建，等价于让 VPU 再有一次
            // 机会——不额外加"死透"标志：此路径的重试节奏已经被 feeder
            // 的循环周期天然限速，不会像 §audioDead_ 那条注释描述的
            // "每包重试"死循环。
            if (vBin_ != nullptr &&
                GST_MESSAGE_SRC(msg) != nullptr &&
                gst_object_has_as_ancestor(GST_MESSAGE_SRC(msg),
                                           GST_OBJECT(vBin_))) {
                log_.write("[media:video] fatal error in video bin, "
                           "tearing down session (will retry on next "
                           "feeder loop)");
                stopVideoSessionLocked();
            }
        }
        if (cbs_.onError) {
            cbs_.onError(what);
        }
        break;
    }
    case GST_MESSAGE_EOS:
        // 两路 appsrc 都 EOS 才会到这（文件源非循环模式）
        log_.write("[media] end of stream");
        break;
    case GST_MESSAGE_WARNING:
        break;  // 协商过程中的可恢复告警不刷日志
    default:
        break;
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
// 诊断：每 2s 打包计数与两级水位，定位数据断点
// ---------------------------------------------------------------------------

gboolean MediaPlayer::diagTickThunk(gpointer data) {
    return static_cast<MediaPlayer*>(data)->diagTick() ? G_SOURCE_CONTINUE
                                                       : G_SOURCE_REMOVE;
}

gboolean MediaPlayer::diagTick() {
    std::lock_guard<std::mutex> lk(mu_);
    guint64 vqTime = 0, aqTime = 0, vqBytes = 0, aqBytes = 0;
    if (vQueue_ != nullptr) {
        g_object_get(vQueue_, "current-level-time", &vqTime,
                     "current-level-bytes", &vqBytes, nullptr);
    }
    if (aQueue_ != nullptr) {
        g_object_get(aQueue_, "current-level-time", &aqTime,
                     "current-level-bytes", &aqBytes, nullptr);
    }
    /*log_.write("[diag] pushed v=%llu a=%llu pkts | v_queue %luus/%luB "
               "a_queue %luus/%luB",
               static_cast<unsigned long long>(pushedVideoPkts_),
               static_cast<unsigned long long>(pushedAudioPkts_),
               static_cast<unsigned long>(vqTime),
               static_cast<unsigned long>(vqBytes),
               static_cast<unsigned long>(aqTime),
               static_cast<unsigned long>(aqBytes));*/
    return TRUE;
}

// ---------------------------------------------------------------------------
// 卡顿检测（§4.2）：视频解码前 queue 水位超阈值 → 等 IDR
// ---------------------------------------------------------------------------

gboolean MediaPlayer::stallTickThunk(gpointer data) {
    return static_cast<MediaPlayer*>(data)->stallTick()
               ? G_SOURCE_CONTINUE
               : G_SOURCE_REMOVE;
}

gboolean MediaPlayer::stallTick() {
    std::lock_guard<std::mutex> lk(mu_);
    // 首帧解码成功前不武装：解码器冷启动（omx 协商/autoplug）需要数秒，
    // 期间 queue 顶满是正常现象，不是卡顿（实测会误触发 IDR 丢弃）
    if (vQueue_ == nullptr || !stallArmed_) {
        return TRUE;
    }
    if (waitingIdr_) {
        // 等 IDR 超时兜底：链路异常时不能永远黑屏（放弃恢复、接受下一包）
        if (g_get_monotonic_time() - waitIdrStartUs_ >
            static_cast<gint64>(opts_.stallGiveUpSec) * 1000000) {
            waitingIdr_ = false;
            log_.write("[media] stall: no IDR within %us, giving up recovery",
                       static_cast<unsigned>(opts_.stallGiveUpSec));
        }
        return TRUE;
    }
    guint64 level = 0;
    g_object_get(vQueue_, "current-level-time", &level, nullptr);
    if (level > profile_.stallThresholdNs) {
        // 持续确认：解码器启动后的追帧期 queue 会瞬时顶满（健康现象），
        // 连续超阈值 2 秒才判卡顿，避免误触发 IDR 丢弃（实测踩过）
        if (stallOverSinceUs_ == 0) {
            stallOverSinceUs_ = g_get_monotonic_time();
        } else if (g_get_monotonic_time() - stallOverSinceUs_ > 2000000) {
            waitingIdr_ = true;
            waitIdrStartUs_ = g_get_monotonic_time();
            stallOverSinceUs_ = 0;
            log_.write("[media] stall detected: video queue %lums > %lums "
                       "for 2s, waiting for IDR",
                       static_cast<unsigned long>(level / 1000000),
                       static_cast<unsigned long>(
                           profile_.stallThresholdNs / 1000000));
            if (cbs_.onStall) {
                cbs_.onStall();  // 上层向协议端请求 IDR（文件源无此通道）
            }
        }
    } else {
        stallOverSinceUs_ = 0;
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
// 音画纠偏（§7）：仅两路并存；小步长只调视频侧 ts-offset
// ---------------------------------------------------------------------------

gboolean MediaPlayer::driftTickThunk(gpointer data) {
    return static_cast<MediaPlayer*>(data)->driftTick()
               ? G_SOURCE_CONTINUE
               : G_SOURCE_REMOVE;
}

gboolean MediaPlayer::driftTick() {
    std::lock_guard<std::mutex> lk(mu_);
    if (vSink_ == nullptr || aSink_ == nullptr) {
        return TRUE;  // 单路：无音画同步问题（§5）
    }
    gint64 vpos = -1;
    gint64 apos = -1;
    gst_element_query_position(vSink_, GST_FORMAT_TIME, &vpos);
    gst_element_query_position(aSink_, GST_FORMAT_TIME, &apos);
    if (vpos < 0 || apos < 0) {
        return TRUE;
    }
    const gint64 drift = vpos - apos;  // >0: 视频渲染位置超前音频
    // 死区：同步误差在 ±20ms 内不动作（时钟读取噪声级）
    const gint64 deadband = 20 * GST_MSECOND;
    if (drift > deadband || drift < -deadband) {
        // §7：drift/10 小步长，夹 ±5ms，避免跳变；只动视频不动音频
        gint64 step = drift / 10;
        if (step > 5 * GST_MSECOND) {
            step = 5 * GST_MSECOND;
        }
        if (step < -5 * GST_MSECOND) {
            step = -5 * GST_MSECOND;
        }
        gint64 cur = 0;
        g_object_get(vSink_, "ts-offset", &cur, nullptr);
        g_object_set(vSink_, "ts-offset", cur + step, nullptr);
        log_.write("[drift] video=%.2fs audio=%.2fs diff=%+.1fms "
                   "step=%+.1fms offset=%+.1fms",
                   vpos / 1000000000.0, apos / 1000000000.0, drift / 1000000.0,
                   step / 1000000.0, (cur + step) / 1000000.0);
    } else {
        log_.write("[drift] video=%.2fs audio=%.2fs diff=%+.1fms (in sync)",
                   vpos / 1000000000.0, apos / 1000000000.0, drift / 1000000.0);
    }
    return TRUE;
}

// ---------------------------------------------------------------------------
// 帧输出（主线程）
// ---------------------------------------------------------------------------

bool MediaPlayer::pullVideoFrame(VideoFrame& out) {
    std::lock_guard<std::mutex> lk(mu_);
    if (vSink_ == nullptr) {
        return false;
    }
    if (opts_.drmOutput) {
        // drm 路径：上一帧 sample 再保一拍。plane 换帧是异步的（下个
        // vblank 生效），过早归还 buffer 会让 omx pool 复写 plane 仍
        // 在扫描的内存（GEM handle 只保内存不保内容）。
        if (prevSample_ != nullptr) {
            gst_sample_unref(prevSample_);
        }
        prevSample_ = sample_;
        sample_ = nullptr;
    } else {
        releaseFrameLocked();
    }

    sample_ = gst_app_sink_pull_sample(GST_APP_SINK(vSink_));
    if (sample_ == nullptr) {
        return false;
    }

    GstCaps* caps = gst_sample_get_caps(sample_);
    GstVideoInfo info{};
    if (caps == nullptr || !gst_video_info_from_caps(&info, caps)) {
        log_.write("[media:video] sample without valid caps, dropped");
        releaseFrameLocked();
        return false;
    }

    GstBuffer* buf = gst_sample_get_buffer(sample_);
    if (buf == nullptr) {
        releaseFrameLocked();
        return false;
    }

    // ---- drm 路径：dmabuf 帧输出 fd 描述（不 map，零拷贝）----
    GstMemory* mem0 = gst_buffer_peek_memory(buf, 0);
    if (opts_.drmOutput && mem0 != nullptr && gst_is_dmabuf_memory(mem0)) {
        frame_ = VideoFrame{};
        frame_.isDmabuf = true;
        frame_.width = GST_VIDEO_INFO_WIDTH(&info);
        frame_.height = GST_VIDEO_INFO_HEIGHT(&info);
        frame_.ptsNs = GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(buf))
                           ? static_cast<std::int64_t>(GST_BUFFER_PTS(buf))
                           : -1;
        frame_.fourcc = gstFormatToDrmFourcc(GST_VIDEO_INFO_FORMAT(&info));
        if (frame_.fourcc == 0) {
            log_.write("[media:video] drm path: unsupported pixel format, "
                       "frame dropped");
            releaseFrameLocked();
            return false;
        }
        // plane 布局：优先 GstVideoMeta；但查证 BSP 源码
        // （sysroot usr/src/debug/gstreamer1.0-omx/.../gstomxvideodec.c）
        // 证实这颗 vendor omx 解码器从不调用 gst_buffer_add_video_meta，
        // 即 vm 在这条链路上恒为 nullptr——pitch 实际总是走下面的
        // GST_VIDEO_INFO_PLANE_STRIDE 分支，按 caps 宽高假设"紧凑无
        // padding"算出来的，不是硬件真实 nStride。若 VPU 对该分辨率有
        // 行对齐 padding，这里会算错 pitch，表现为逐行错位的"画面不纯
        // 净"（且不会被 AddFB2 拒绝——pitch 只影响采样位置，不影响
        // ioctl 成功与否）。下方 first-frame 日志打印了 GstBuffer 实际
        // 字节数，可用来核对是否存在 padding（NV12 无 padding 时应
        // 等于 width*height*3/2）。
        // 每 plane fd：buffer 按 plane 分 memory 时取各自 fd，否则共用
        // 首 memory 的 fd（单 fd 双 plane 场景，offset 区分）。
        const GstVideoMeta* vm = gst_buffer_get_video_meta(buf);
        const std::uint32_t nPlanes =
            vm != nullptr ? vm->n_planes
                          : GST_VIDEO_INFO_N_PLANES(&info);
        const std::uint32_t nMem = gst_buffer_n_memory(buf);
        frame_.numPlanes = static_cast<int>(nPlanes);
        for (std::uint32_t p = 0; p < nPlanes && p < 4; ++p) {
            if (p < nMem) {
                GstMemory* mem = gst_buffer_peek_memory(buf, p);
                frame_.fd[p] = (mem != nullptr && gst_is_dmabuf_memory(mem))
                                   ? gst_dmabuf_memory_get_fd(mem)
                                   : gst_dmabuf_memory_get_fd(mem0);
            } else {
                frame_.fd[p] = gst_dmabuf_memory_get_fd(mem0);
            }
            if (vm != nullptr) {
                frame_.offset[p] = vm->offset[p];
                frame_.pitch[p] = vm->stride[p];
            } else {
                // vm 恒为 nullptr（见上方注释），此前这里漏了 offset 的
                // 赋值：chroma plane（p=1）的 offset 一直留在默认值 0，
                // 等于告诉 plane "UV 数据从 buffer 起始处开始"——实际
                // 那里是 Y 平面数据，显示硬件把亮度字节当色度采样，
                // 表现为强烈的绿色偏色（"画面不纯净"真正成因，不是
                // BGR/RGB 反转——这条链路全程是 YUV，从没转换成
                // RGB）。用 GST_VIDEO_INFO_PLANE_OFFSET 补上，与上面
                // PLANE_STRIDE 同一套 GstVideoInfo 紧凑布局假设一致。
                frame_.offset[p] =
                    static_cast<std::uint32_t>(GST_VIDEO_INFO_PLANE_OFFSET(&info, p));
                frame_.pitch[p] =
                    GST_VIDEO_INFO_PLANE_STRIDE(&info, p);
            }
        }
        // modifier：GStreamer 1.22 caps/video-meta 均不携带，按配置注入。
        // 2026-09-18 查证 BSP OMX 源码（omx_vpudec_component.c：WAVE 核心
        // wtlEnable 恒为 TRUE，硬件 Write-To-Linear；gstomxvideodec.c：
        // 仅 caps 带 GST_CAPS_FEATURE_MEMORY_IFBC 才切 IFBC32x8Tiled 输出，
        // 本管线协商的纯 DMABuf caps 不带该 feature）+ `modetest -M
        // semidrive -p` 实测 plane NV12 IN_FORMATS 列表，确认 use-dmabuf
        // 输出是 WTL 后的真 LINEAR NV12，modifier 应为 0（曾按 WAVE_32X8_
        // TILE/FBDC 猜测注入非零值，被 plane 逐帧拒绝，靠回退路径侥幸
        // 显示——细节见 config/androidauto.ini drm_modifier 注释）。
        frame_.modifier = opts_.drmModifier;

        if (!stallArmed_) {
            stallArmed_ = true;
            log_.write("[media:video] first dmabuf frame decoded (%dx%d, "
                       "fourcc=0x%08x, planes=%d, buf_bytes=%zu, "
                       "tight_nv12_bytes=%zu, pitch0=%u, offset1=%u), "
                       "stall monitor armed",
                       static_cast<int>(frame_.width),
                       static_cast<int>(frame_.height), frame_.fourcc,
                       frame_.numPlanes,
                       gst_buffer_get_size(buf),
                       static_cast<std::size_t>(frame_.width) *
                           static_cast<std::size_t>(frame_.height) * 3 / 2,
                       frame_.pitch[0], frame_.offset[1]);
        }
        out = frame_;
        return true;
    }
    if (opts_.drmOutput && mem0 != nullptr) {
        // drm 模式但输出非 dmabuf（软解兜底/属性缺失）：记日志，
        // 按 shm 帧走 map 路径，上层 cairo 兜底绘制
        log_.write("[media:video] drm path: decoder gave system memory, "
                   "falling back to mapped frame");
    }

    if (!gst_buffer_map(buf, &map_, GST_MAP_READ)) {
        releaseFrameLocked();
        return false;
    }
    mappedBuf_ = buf;

    frame_.data = static_cast<const std::uint8_t*>(map_.data);
    frame_.width = GST_VIDEO_INFO_WIDTH(&info);
    frame_.height = GST_VIDEO_INFO_HEIGHT(&info);
    frame_.stride = GST_VIDEO_INFO_PLANE_STRIDE(&info, 0);
    frame_.ptsNs = GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(buf))
                       ? static_cast<std::int64_t>(GST_BUFFER_PTS(buf))
                       : -1;

    // 首帧解码成功：解码链已就绪，此后 queue 顶满才真的是卡顿
    if (!stallArmed_) {
        stallArmed_ = true;
        log_.write("[media:video] first frame decoded (%dx%d), stall "
                   "monitor armed",
                   static_cast<int>(frame_.width),
                   static_cast<int>(frame_.height));
    }

    out = frame_;
    return true;
}

void MediaPlayer::releaseFrameLocked() {
    if (prevSample_ != nullptr) {
        gst_sample_unref(prevSample_);
        prevSample_ = nullptr;
    }
    if (mappedBuf_ != nullptr) {
        gst_buffer_unmap(mappedBuf_, &map_);
        mappedBuf_ = nullptr;
    }
    if (sample_ != nullptr) {
        gst_sample_unref(sample_);
        sample_ = nullptr;
    }
    frame_ = VideoFrame{};
}

std::int64_t MediaPlayer::takeReadTimeUs(std::int64_t ptsNs) {
    std::lock_guard<std::mutex> lk(mu_);
    const auto it = traceRead_.find(ptsNs);
    if (it == traceRead_.end()) {
        return 0;
    }
    const std::int64_t t = it->second;
    const auto after = std::next(it);
    traceRead_.erase(traceRead_.begin(), after);
    return t;
}

} // namespace aa
