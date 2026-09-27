#include "media/FilePacketFeeder.h"

#include <cstring>

namespace aa {

FilePacketFeeder::FilePacketFeeder(Log& log, std::string uri, Hooks hooks)
    : log_(log), uri_(std::move(uri)), hooks_(std::move(hooks)) {}

FilePacketFeeder::~FilePacketFeeder() {
    stop();
}

bool FilePacketFeeder::start() {
    if (pipeline_ != nullptr) {
        return true;
    }
    pipeline_ = gst_pipeline_new("feeder-pipeline");
    GstElement* src = gst_element_factory_make("filesrc", "feeder-src");
    GstElement* parse = gst_element_factory_make("parsebin", "feeder-parse");
    if (pipeline_ == nullptr || src == nullptr || parse == nullptr) {
        log_.write("[feeder] element create failed");
        if (pipeline_ != nullptr) {
            gst_object_unref(pipeline_);
            pipeline_ = nullptr;
        }
        return false;
    }
    // filesrc 要裸路径；AppConfig 把绝对路径规范化成了 file:// URI，剥掉
    std::string path = uri_;
    if (path.compare(0, 7, "file://") == 0) {
        path = path.substr(7);
    }
    g_object_set(src, "location", path.c_str(), nullptr);

    gst_bin_add_many(GST_BIN(pipeline_), src, parse, nullptr);
    if (!gst_element_link(src, parse)) {
        log_.write("[feeder] filesrc ! parsebin link failed");
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
        return false;
    }
    // parsebin 输出 pad 到达时动态挂 fakesink（pad-added）
    g_signal_connect(parse, "pad-added",
                     G_CALLBACK(&FilePacketFeeder::padAddedThunk), this);

    GstBus* bus = gst_element_get_bus(pipeline_);
    gst_bus_add_watch(bus, &FilePacketFeeder::busThunk, this);
    gst_object_unref(bus);

    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE) {
        log_.write("[feeder] set_state(PLAYING) failed");
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
        return false;
    }
    log_.write("[feeder] started, uri=%s", uri_.c_str());
    return true;
}

void FilePacketFeeder::stop() {
    if (pipeline_ != nullptr) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
        log_.write("[feeder] stopped");
    }
}

// ---------------------------------------------------------------------------
// parsebin 输出 pad → 逐 pad 挂 fakesink（handoff 桥）
// ---------------------------------------------------------------------------

void FilePacketFeeder::padAddedThunk(GstElement* /*parsebin*/, GstPad* pad,
                                     gpointer data) {
    static_cast<FilePacketFeeder*>(data)->onPadAdded(pad);
}

void FilePacketFeeder::onPadAdded(GstPad* pad) {
    // pad 的流类型到 handoff 时才明确（pad-added 时 caps 可能为空），
    // 这里只负责给每个 pad 接一个带 handoff 的 fakesink
    GstElement* fs = gst_element_factory_make("fakesink", nullptr);
    if (fs == nullptr) {
        return;
    }
    // sync=TRUE：按文件 PTS 以 1x 重放（等价投屏包的实时到达节奏）。
    // 若 sync=FALSE，整文件会被瞬间灌进下游 appsrc 的内部队列，
    // PTS 越过管线时钟一整轮，播放全乱（实测）。
    g_object_set(fs, "sync", TRUE, "signal-handoffs", TRUE, "silent", TRUE,
                 nullptr);
    g_signal_connect(fs, "handoff", G_CALLBACK(&FilePacketFeeder::handoffThunk),
                     this);

    gst_bin_add(GST_BIN(pipeline_), fs);
    gst_element_sync_state_with_parent(fs);

    GstPad* sinkpad = gst_element_get_static_pad(fs, "sink");
    if (sinkpad == nullptr ||
        gst_pad_link(pad, sinkpad) != GST_PAD_LINK_OK) {
        log_.write("[feeder] pad link failed");
    }
    if (sinkpad != nullptr) {
        gst_object_unref(sinkpad);
    }
}

void FilePacketFeeder::handoffThunk(GstElement* /*fakesink*/, GstBuffer* buf,
                                    GstPad* pad, gpointer data) {
    static_cast<FilePacketFeeder*>(data)->onHandoff(buf, pad);
}

void FilePacketFeeder::onHandoff(GstBuffer* buf, GstPad* pad) {
    GstCaps* caps = gst_pad_get_current_caps(pad);
    if (caps == nullptr) {
        caps = gst_pad_query_caps(pad, nullptr);
    }
    const GstStructure* s =
        caps != nullptr ? gst_caps_get_structure(caps, 0) : nullptr;
    const bool isVideo =
        s != nullptr && g_str_has_prefix(gst_structure_get_name(s), "video/");
    const bool isAudio =
        s != nullptr && g_str_has_prefix(gst_structure_get_name(s), "audio/");
    const bool wantVideo = isVideo && hooks_.onVideo;
    const bool wantAudio = isAudio && hooks_.onAudio;
    if (caps != nullptr && !wantVideo && !wantAudio) {
        gst_caps_unref(caps);  // 无消费方的流（如字幕）直接丢弃
        return;
    }

    gsize size = gst_buffer_get_size(buf);
    if (size == 0 || caps == nullptr) {
        if (caps != nullptr) {
            gst_caps_unref(caps);
        }
        return;
    }
    auto* data = static_cast<std::uint8_t*>(g_malloc(size));
    gst_buffer_extract(buf, 0, data, size);

    const bool hasPts = GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(buf));
    const bool hasDts = GST_CLOCK_TIME_IS_VALID(GST_BUFFER_DTS(buf));
    const std::uint64_t pts =
        hasPts ? GST_BUFFER_PTS(buf) + ptsOffsetNs_ : 0;
    const std::uint64_t dts =
        hasDts ? GST_BUFFER_DTS(buf) + ptsOffsetNs_ : 0;
    // parsebin 输出已按 AU 对齐：非关键帧带 DELTA_UNIT 标记
    const bool isIdr =
        !GST_BUFFER_FLAG_IS_SET(buf, GST_BUFFER_FLAG_DELTA_UNIT);

    if (wantVideo) {
        hooks_.onVideo(data, size, pts, dts, hasDts, isIdr, caps);
    } else if (wantAudio) {
        hooks_.onAudio(data, size, pts, caps);
    }
    g_free(data);
    gst_caps_unref(caps);
}

// ---------------------------------------------------------------------------
// 总线：EOS → 循环（偏移累加 + seek 0）或上抛
// ---------------------------------------------------------------------------

gboolean FilePacketFeeder::busThunk(GstBus* /*bus*/, GstMessage* msg,
                                    gpointer data) {
    auto* self = static_cast<FilePacketFeeder*>(data);
    return self->busWatch(msg) ? TRUE : FALSE;
}

gboolean FilePacketFeeder::busWatch(GstMessage* msg) {
    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
        GError* err = nullptr;
        gchar* dbg = nullptr;
        gst_message_parse_error(msg, &err, &dbg);
        log_.write("[feeder] error: %s (%s)",
                   err != nullptr ? err->message : "?",
                   dbg != nullptr ? dbg : "-");
        g_clear_error(&err);
        g_free(dbg);
        break;
    }
    case GST_MESSAGE_EOS:
        if (loop_ && pipeline_ != nullptr) {
            // 循环 = 整管线重建（不是 seek）：parsebin 对 FLUSH seek 的
            // 支持不可靠（实测 seek 返回成功但流不再产出）。重建是毫秒
            // 级开销；ptsOffset 累计保持下游 live 管线的 PTS 单调连续。
            gint64 dur = 0;
            gst_element_query_duration(pipeline_, GST_FORMAT_TIME, &dur);
            if (dur > 0) {
                ptsOffsetNs_ += static_cast<std::uint64_t>(dur);
            }
            const bool wantLoop = loop_;
            gst_element_set_state(pipeline_, GST_STATE_NULL);
            gst_object_unref(pipeline_);
            pipeline_ = nullptr;
            if (wantLoop) {
                log_.write("[feeder] loop: restart (pts offset +%lldms)",
                           static_cast<long long>(dur / 1000000));
                start();  // 重建（baseTs 语义由 offset 延续）
            }
        } else {
            log_.write("[feeder] end of stream");
            if (hooks_.onEos) {
                hooks_.onEos();
            }
        }
        break;
    default:
        break;
    }
    return TRUE;
}

} // namespace aa
