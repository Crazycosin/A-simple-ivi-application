#include "app/AndroidAutoApp.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <functional>
#include <thread>
#include <tuple>
#include <utility>

#include <sys/eventfd.h>
#include <unistd.h>

#include <gio/gio.h>
#include <glib-unix.h>
#include <gst/gst.h>

#include "media/FilePacketFeeder.h"
#include "media/MediaPlayer.h"
#include "util/UsbDeviceMonitor.h"

namespace aa {
namespace {

bool SameUsbStatus(const UsbStatus& lhs, const UsbStatus& rhs) {
  if (lhs.state != rhs.state || lhs.message != rhs.message ||
      lhs.devices.size() != rhs.devices.size()) return false;
  for (std::size_t i = 0; i < lhs.devices.size(); ++i) {
    const auto& a = lhs.devices[i].device;
    const auto& b = rhs.devices[i].device;
    if (std::tie(a.bus, a.address, a.vendor_id, a.product_id, a.speed, a.manufacturer,
                 a.product, a.aoa_version, a.query_error) !=
        std::tie(b.bus, b.address, b.vendor_id, b.product_id, b.speed, b.manufacturer,
                 b.product, b.aoa_version, b.query_error) ||
        a.interfaces.size() != b.interfaces.size()) return false;
    for (std::size_t j = 0; j < a.interfaces.size(); ++j) {
      if (a.interfaces[j].device_class != b.interfaces[j].device_class ||
          a.interfaces[j].subclass != b.interfaces[j].subclass) return false;
    }
    const auto& ar = lhs.devices[i].recognition;
    const auto& br = rhs.devices[i].recognition;
    if (std::tie(ar.is_phone, ar.aa_candidate, ar.reason) !=
        std::tie(br.is_phone, br.aa_candidate, br.reason)) return false;
  }
  return true;
}

void WatchUsbStatusChanges(const std::atomic<bool>& stop, int notification_fd) {
  UsbStatus previous;
  while (!stop.load()) {
    auto current = UsbDeviceMonitor::Instance().GetStatus();
    if (!SameUsbStatus(previous, current)) {
      previous = std::move(current);
      const std::uint64_t value = 1;
      while (write(notification_fd, &value, sizeof(value)) < 0 && errno == EINTR) {}
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
}

}  // namespace

int AndroidAutoApp::run(const std::string& exeDir) {
    gst_init(nullptr, nullptr);

    Config cfg;
    std::string iniPath;
    std::size_t cfgErrs = 0;
    const bool hasIni = AppConfig::load(exeDir, cfg, iniPath, cfgErrs);

    AndroidAutoApp app(std::move(cfg));
    if (!app.setup(iniPath, hasIni, cfgErrs)) {
        return 2;
    }
    app.mainLoop();
    return 0;
}

AndroidAutoApp::AndroidAutoApp(Config cfg)
    : cfg_(std::move(cfg)), inputCtrl_(cfg_, model_, log_) {}

bool AndroidAutoApp::setup(const std::string& iniPath, bool hasIni,
                            std::size_t cfgErrs) {
    if (hasIni) {
        log_.open(cfg_.logPath);
        log_.write("[config] loaded %s (%zu invalid keys skipped)",
                   iniPath.c_str(), cfgErrs);
    } else {
        log_.open(std::string());
        log_.write("[config] ini not found, built-in defaults in use");
    }
    tStartUs_ = g_get_monotonic_time();

    // hmi-controller 绑定按需开启：定制版 bind 有权限校验（详见
    // README「布局控制路径」），未配套时绑定本身即 fatal，默认不绑
    wl_.enableHmiController(cfg_.player.fullscreenLayout);

    if (!wl_.connect()) {
        std::fprintf(stderr, "wayland: %s\n", wl_.lastError().c_str());
        log_.write("[fatal] %s", wl_.lastError().c_str());
        return false;
    }
    if (!wl_.globalsReady()) {
        const char* msg = "missing globals: need wl_compositor + wl_shm + "
                          "ivi_application (is ivi-shell running?)";
        std::fprintf(stderr, "wayland: %s\n", msg);
        log_.write("[fatal] %s", msg);
        return false;
    }
    if (!wl_.createIviSurface(cfg_.surfaceId)) {
        std::fprintf(stderr, "wayland: %s\n", wl_.lastError().c_str());
        log_.write("[fatal] %s", wl_.lastError().c_str());
        return false;
    }
    log_.write("[app] ivi surface id=%u", static_cast<unsigned>(cfg_.surfaceId));

    wl_.onConfigure = [this](std::int32_t w, std::int32_t h) {
        inputCtrl_.onConfigure(w, h);
    };
    wl_.onPointerButton = [this](std::int32_t x, std::int32_t y, bool p) {
        inputCtrl_.onPointerButton(x, y, p);
    };
    wl_.onTouch = [this](std::int32_t x, std::int32_t y, TouchPhase ph) {
        inputCtrl_.onTouch(x, y, ph);
    };

    view_ = std::make_unique<AppView>(cfg_);

    // 先切 full_screen 布局再提交首帧：hmi-controller 在 surface 首次
    // 有内容时才把它挂进 application layer 并按当前布局模式排 rect，
    // 提前切换可让首帧 configure 直接给全屏区域（免去 tiling 闪变）。
    if (cfg_.player.fullscreenLayout) {
        if (wl_.switchLayout(HmiLayoutMode::FullScreen)) {
            layoutSwitched_ = true;
            log_.write("[hmi] layout -> full_screen");
        } else {
            log_.write("[hmi] switch to full_screen failed (%s), "
                       "staying tiling",
                       wl_.lastError().c_str());
        }
    }

    // 媒体：统一管线 + 文件拆包桥（audio_uri 空时同一文件出两路包）
    if (cfg_.player.enabled && !cfg_.player.videoUri.empty()) {
        // DRM 直扫分支（render_path=drm）：先建 DrmView（失败自动回退
        // shm 绘制路径），媒体管线按 drmOutput 决定视频链形态
        if (cfg_.player.drmRender) {
            DrmView::Options dvo;
            dvo.device = cfg_.player.drmDevice;
            dvo.planeId = cfg_.player.drmPlaneId;
            if (drmView_.create(log_, dvo)) {
                drmView_.setViewport(cfg_.player.drmX, cfg_.player.drmY,
                                     cfg_.player.drmW, cfg_.player.drmH);
                drmActive_ = true;
                // 光标可见性（调试用）：weston 没有硬件光标 plane、鼠标
                // 是软件合成进它自己 primary 画面的，视频 plane 一盖就
                // 挡住了，跟视频 zpos 数值无关（2026-09-18 排查结论）。
                // 另开一路独立 plane 画箭头，zpos 压在视频（2）之上；
                // 失败（没有空闲 ARGB overlay plane）只记日志，不影响
                // 视频路径。整个功能可删（CursorOverlay 类注释里写清楚
                // 了要动哪几处）。
                cursor_.create(log_, cfg_.player.drmDevice, drmView_.crtcId(),
                               drmView_.planeId());
                // 视频区输入通道：视频在 overlay plane 上，weston 感知
                // 不到该区域，直读 evdev（多读者，与 weston 并行不抢夺）
                EvdevReader::Options eo;
                eo.screenW = cfg_.player.evdevScreenW;
                eo.screenH = cfg_.player.evdevScreenH;
                evdev_.start(
                    log_, eo,
                    [this](std::int32_t x, std::int32_t y, std::int32_t b,
                           bool p) { onEvdevPointer(x, y, b, p); },
                    [this](std::int32_t x, std::int32_t y) {
                        cursor_.moveTo(x, y);
                    });
            } else {
                log_.write("[drm] view create failed, falling back to "
                           "shm/cairo path");
            }
        }

        MediaPlayer::Options mo;
        mo.transport = cfg_.player.transport;
        if (cfg_.player.queueMaxMs > 0) {
            mo.queueMaxNsOverride =
                static_cast<std::uint64_t>(cfg_.player.queueMaxMs) * 1000000u;
        }
        if (cfg_.player.stallThresholdMs > 0) {
            mo.stallThresholdNsOverride =
                static_cast<std::uint64_t>(cfg_.player.stallThresholdMs) *
                1000000u;
        }
        mo.driftIntervalSec = cfg_.player.driftIntervalSec;
        // 卡顿检测+IDR 恢复是投屏网络卡顿机制（需向发送端请求 IDR 的
        // 通道）；文件源必须关（误触发会重置 base_ts 打乱音画时间线）
        mo.stallDetect = cfg_.player.stallDetect;
        mo.drmOutput = drmActive_;
        mo.drmModifier = cfg_.player.drmModifier;

        MediaPlayer::Callbacks cbs;
        cbs.onVideoFrame = [this] { onVideoFrame(); };
        cbs.onStall = [this] {
            log_.write("[media] stall: waiting for IDR "
                       "(file source has no IDR request channel)");
        };
        cbs.onError = [this](const std::string& what) {
            log_.write("[media] pipeline error: %s", what.c_str());
        };
        media_ = std::make_unique<MediaPlayer>(log_, std::move(cbs), mo);
        if (!media_->start()) {
            media_.reset();
        } else {
            const bool sameAudio = cfg_.player.audioUri.empty() ||
                                   cfg_.player.audioUri == cfg_.player.videoUri;

            FilePacketFeeder::Hooks vh;
            vh.onVideo = [this](const std::uint8_t* d, std::size_t s,
                                std::uint64_t pts, std::uint64_t dts,
                                bool hasDts, bool isIdr, const GstCaps* caps) {
                media_->setVideoCaps(caps);
                media_->pushVideoPacket(d, s, pts, dts, hasDts, isIdr);
            };
            if (sameAudio) {
                vh.onAudio = [this](const std::uint8_t* d, std::size_t s,
                                    std::uint64_t pts, const GstCaps* caps) {
                    if (!cfg_.player.audioEnable) {
                        return;  // 纯视频模式：音频包直接丢弃（不出会话）
                    }
                    media_->setAudioCaps(caps);
                    media_->pushAudioPacket(d, s, pts);
                };
            }
            vh.onEos = [this] {
                // 非循环模式：整文件结束。此前这里只发 EOS 给管线、应用
                // 继续常驻在末帧——DRM 直扫模式下视频 plane（zpos=2、
                // blend=none 纯覆盖）从未被清掉，会永久挡住下层 weston
                // 画面，移动鼠标触发的 weston 重绘也无济于事（重绘的是
                // 被挡住的下层）。实测证实：这不是用户想要的"播放结束
                // 回到 launcher"效果。改为直接走 model_.quit()——跟
                // ✕ 按钮/SIGTERM 完全同一条已验证过的退出路径
                // （teardown() 里关 plane、触发 repaint、恢复 tiling
                // 布局都在这条路径上），不新增一套"仅停媒体不退出"的
                // 半途状态。
                media_->endVideoStream();
                if (audioFeeder_ == nullptr) {
                    media_->endAudioStream();
                }
                model_.quit();
            };
            videoFeeder_ = std::make_unique<FilePacketFeeder>(
                log_, cfg_.player.videoUri, std::move(vh));
            videoFeeder_->setLoop(cfg_.player.loop);
            videoFeeder_->start();

            if (!sameAudio && cfg_.player.audioEnable) {
                FilePacketFeeder::Hooks ah;
                ah.onAudio = [this](const std::uint8_t* d, std::size_t s,
                                    std::uint64_t pts, const GstCaps* caps) {
                    media_->setAudioCaps(caps);
                    media_->pushAudioPacket(d, s, pts);
                };
                audioFeeder_ = std::make_unique<FilePacketFeeder>(
                    log_, cfg_.player.audioUri, std::move(ah));
                audioFeeder_->setLoop(cfg_.player.loop);
                audioFeeder_->start();
            }
        }
    }

    // 首帧：用 ini 兜底尺寸提交内容。
    // ivi hmi-controller 需要 surface 有内容后才将其归入 application layer，
    // 随后 configure 事件会给实际尺寸触发重绘。
    //model_.setWindowSize(cfg_.defaultWidth, cfg_.defaultHeight);
    repaint();
    model_.takeDirty();  // 首帧已画，清掉标记
    return true;
}

void AndroidAutoApp::mainLoop() {
    loop_ = g_main_loop_new(nullptr, FALSE);

    StartUsbMonitoring();
    PrintUsbStatus();

    // Watch cached USB state; perform printing on the GLib main thread.
    const int usb_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    std::atomic<bool> stop_usb_watch{false};
    std::thread usb_watch;
    guint usb_watch_source = 0;
    if (usb_fd >= 0) {
        usb_watch_source = g_unix_fd_add(
            usb_fd, G_IO_IN, [](gint fd, GIOCondition, gpointer data) -> gboolean {
                std::uint64_t value;
                while (read(fd, &value, sizeof(value)) < 0 && errno == EINTR) {}
                static_cast<AndroidAutoApp*>(data)->PrintUsbStatus();
                return G_SOURCE_CONTINUE;
            }, this);
        usb_watch = std::thread(WatchUsbStatusChanges, std::cref(stop_usb_watch), usb_fd);
    } else {
        log_.write("[usb] cannot start status watcher: eventfd errno=%d", errno);
    }

    g_unix_fd_add(wl_.displayFd(), G_IO_IN, &AndroidAutoApp::onWlReadableThunk,
                  this);
    // SIGTERM/SIGINT → 优雅退出（跑 teardown：停管线、时延总结、
    // 布局恢复）；不接的话 kill 直接终止，这些清理都做不了
    g_unix_signal_add(SIGTERM, &AndroidAutoApp::onSignalThunk, this);
    g_unix_signal_add(SIGINT, &AndroidAutoApp::onSignalThunk, this);
    // 退出检查兜底（常规路径：close 点击是 wayland 输入事件，dispatch
    // 返回后即退出；此定时器覆盖无 wayland 事件来源的 quit）
    g_timeout_add(200, &AndroidAutoApp::onQuitCheckThunk, this);

    g_main_loop_run(loop_);
    stop_usb_watch.store(true);
    if (usb_watch.joinable()) usb_watch.join();
    if (usb_watch_source != 0) g_source_remove(usb_watch_source);
    if (usb_fd >= 0) close(usb_fd);
    StopUsbMonitoring();
    g_main_loop_unref(loop_);
    loop_ = nullptr;

    log_.write("[app] exit");
    teardown();
}

void AndroidAutoApp::StartUsbMonitoring() {
    UsbDeviceMonitor::Instance().Start();
}

void AndroidAutoApp::StopUsbMonitoring() {
    UsbDeviceMonitor::Instance().Stop();
}

void AndroidAutoApp::PrintUsbStatus() {
    const auto status = UsbDeviceMonitor::Instance().GetStatus();
    const char* state = "stopped";
    switch (status.state) {
        case UsbStatus::State::kStopped: state = "stopped"; break;
        case UsbStatus::State::kStarting: state = "starting"; break;
        case UsbStatus::State::kMonitoring: state = "monitoring"; break;
        case UsbStatus::State::kError: state = "error"; break;
    }
    const char* connection = status.state == UsbStatus::State::kMonitoring
        ? (status.devices.empty() ? "disconnected" : "connected") : "unknown";
    log_.write("[usb] controller=31260000.dwc3 state=%s connection=%s "
               "devices=%zu message=%s",
               state, connection, status.devices.size(), status.message.c_str());
    for (const auto& entry : status.devices) {
        const auto& device = entry.device;
        log_.write("[usb] bus=%d address=%d VID=%04x PID=%04x "
                   "speed=%s manufacturer=%s product=%s interfaces=%zu "
                   "phone=%d aoa_version=%d aa_candidate=%d reason=%s error=%s",
                   device.bus, device.address,
                   static_cast<unsigned int>(device.vendor_id),
                   static_cast<unsigned int>(device.product_id),
                   UsbSpeedName(device.speed), device.manufacturer.c_str(), device.product.c_str(),
                   device.interfaces.size(), entry.recognition.is_phone,
                   device.aoa_version, entry.recognition.aa_candidate,
                   entry.recognition.reason.c_str(), device.query_error.c_str());
    }
}

void AndroidAutoApp::teardown() {
    // feeder 先停（其流线程可能阻塞在 appsrc push 反压上）
    videoFeeder_.reset();
    audioFeeder_.reset();

    // DRM 分支收尾必须在 media_->stop() 之前：DrmView 的 current_/prev_
    // 通过 drmPrimeFDToHandle 对 VPU 输出的 dmabuf 持有独立于 GStreamer
    // 引用计数之外的 GEM 句柄引用。实测（2026-09-18）非循环播放到
    // 片尾后 media_->stop() 会卡死在 gst_element_set_state(..., NULL)——
    // v_dec:src 等所有 streaming 线程全部阻塞在 futex_wait，SIGTERM 都
    // 唤不醒，只能 SIGKILL；dmesg 同时能看到内核
    // `WARNING: .../dma-buf.c:116 dma_buf_release` 反复出现。推断：
    // vendor OMX/VPU 驱动的关闭流程要等自己导出的 dmabuf 被完全释放
    // （所有 importer 都 detach）才能完成状态切换，而 DrmView 持有的
    // GEM 句柄这时还没释放（clear()/destroy() 原来排在 media_->stop()
    // 之后）——OMX 驱动在等一个永远不会来的释放信号，死锁。调整顺序：
    // 先关 plane + GEM_CLOSE 归还所有 dmabuf 引用，再让 OMX 解码器关闭，
    // 此时它自己导出的 buffer 已经没有外部 importer，状态切换才能走完。
    //
    // 先停 evdev（不再收输入），关自己的 plane（不动 weston primary/
    // 不 modeset），最后 commit 一帧 damage 让 weston 立即重绘——否则
    // 被视频盖住的 launcher 区域要等下一次输入事件才恢复（实测：
    // kmssink 退出后必须动鼠标界面才回来）。
    if (drmActive_) {
        evdev_.stop();
        cursor_.destroy();
        drmView_.clear();
        drmView_.destroy();
        drmActive_ = false;
        if (model_.hasSize()) {
            repaint();  // commit 带 damage 的新帧，触发 weston repaint
            wl_.flush();
        }
        log_.write("[drm] plane cleared, weston repaint triggered");
    }

    if (media_ != nullptr) {
        // 看门狗：上面的顺序调整（R9.7）能修掉一类死锁根因，但实测
        // vendor OMX/VPU 关闭流程本身不可靠——同样的代码，命令行测试
        // 跑几次能干净退出，从 launcher 真实点击启动的那次又卡死在
        // gst_element_set_state(..., NULL)（栈证据同上，v_dec:src 等
        // streaming 线程全部 futex_wait），说明这不是我们代码能百分百
        // 消除的确定性 bug，而是闭源驱动的偶发行为。此刻 DRM plane 和
        // 自己的 wl_surface 输入（evdev）已经在上面关掉，唯一还可能
        // 卡住 App 退出、进而卡住 wl_surface 断连（launcher 里那个
        // 带 X 的窗口消失）和下次重新绑定 ivi_surface id 的，就是接下来
        // 这一句。开一个独立线程限时看门狗：超时直接 _exit()，让内核
        // 强制回收——不给它继续拖住 ivi_surface id 阻塞下次启动的机会。
        std::thread watchdog([this] {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            static const char kMsg[] =
                "[app] teardown watchdog: media stop exceeded timeout, "
                "forcing exit\n";
            ssize_t written = ::write(STDERR_FILENO, kMsg, sizeof(kMsg) - 1);
            (void)written;  // best-effort 诊断输出，即将 _exit，不处理失败
            _exit(1);
        });
        watchdog.detach();
        media_->stop();
    }
    logLatencySummary();

    if (cfg_.player.restoreLayout && layoutSwitched_) {
        if (wl_.switchLayout(HmiLayoutMode::Tiling)) {
            wl_.flush();
            log_.write("[hmi] layout -> tiling (restored)");
        }
    }
}

// ---------------------------------------------------------------------------
// glib 事件源
// ---------------------------------------------------------------------------

gboolean AndroidAutoApp::onWlReadableThunk(gint, GIOCondition,
                                           gpointer data) {
    auto* self = static_cast<AndroidAutoApp*>(data);
    if (self->wl_.dispatch() < 0) {
        self->log_.write("[wl] dispatch error, exiting");
        if (self->loop_ != nullptr) {
            g_main_loop_quit(self->loop_);
        }
        return FALSE;
    }
    self->wl_.flush();
    if (!self->model_.running()) {
        if (self->loop_ != nullptr) {
            g_main_loop_quit(self->loop_);
        }
        return FALSE;
    }
    self->maybeRepaint();
    return TRUE;
}

gboolean AndroidAutoApp::onSignalThunk(gpointer data) {
    auto* self = static_cast<AndroidAutoApp*>(data);
    self->log_.write("[app] signal received, quitting");
    if (self->loop_ != nullptr) {
        g_main_loop_quit(self->loop_);
    }
    return G_SOURCE_REMOVE;
}

gboolean AndroidAutoApp::onQuitCheckThunk(gpointer data) {
    auto* self = static_cast<AndroidAutoApp*>(data);
    if (!self->model_.running()) {
        if (self->loop_ != nullptr) {
            g_main_loop_quit(self->loop_);
        }
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

// ---------------------------------------------------------------------------
// DRM 直扫分支：dmabuf 帧 → plane 提交；evdev 视频区坐标上报
// ---------------------------------------------------------------------------

bool AndroidAutoApp::submitDrmFrame(const VideoFrame& f) {
    DrmView::ImageData img;
    for (int p = 0; p < f.numPlanes && p < 4; ++p) {
        img.fd[p] = f.fd[p];
        img.offset[p] = f.offset[p];
        img.pitch[p] = f.pitch[p];
    }
    img.numPlanes = f.numPlanes;
    img.fourcc = f.fourcc;
    img.modifier = f.modifier;
    img.width = f.width;
    img.height = f.height;
    return drmView_.setImageData(img);
}

void AndroidAutoApp::onEvdevPointer(std::int32_t x, std::int32_t y,
                                    std::int32_t button, bool pressed) {
    // 视频区命中判断 + 坐标上报（协议层预留接口）：
    // 命中 viewport 时上报屏幕坐标与视频区内相对坐标（归一化到视频
    // 分辨率，供投屏协议层注入手机侧触摸事件）。
    const bool hit = drmView_.hitTest(x, y);
    if (hit) {
        std::int32_t vx = 0, vy = 0;
        std::int32_t vw = currentFrame_.width;
        std::int32_t vh = currentFrame_.height;
        if (vw > 0 && vh > 0 && drmView_.viewportW() > 0 &&
            drmView_.viewportH() > 0) {
            // viewport 内相对坐标 → 视频像素坐标（投屏注入用）
            vx = (x - drmView_.viewportX()) * (vw - 1) /
                 (drmView_.viewportW() - 1);
            vy = (y - drmView_.viewportY()) * (vh - 1) /
                 (drmView_.viewportH() - 1);
        }
        log_.write("[evdev] video-area click: screen=(%d,%d) video=(%d,%d) "
                   "button=%d pressed=%d",
                   x, y, vx, vy, button, pressed ? 1 : 0);
        // TODO(投屏协议层接入)：将 (vx, vy, button, pressed) 送入协议
        // 层的触摸/按键注入通道（request_idr 同属该通道家族）。
    } else if (cfg_.player.latencyTrace) {
        log_.write("[evdev] click outside video viewport: (%d,%d)", x, y);
    }
}

// ---------------------------------------------------------------------------
// 帧路径：appsink idle 通知 → 拉帧 → 重绘
// ---------------------------------------------------------------------------

void AndroidAutoApp::onVideoFrame() {
    VideoFrame f;
    if (media_ == nullptr || !media_->pullVideoFrame(f)) {
        return;
    }
    currentFrame_ = f;
    model_.setVideoSize(f.width, f.height);

    // DRM 直扫分支：dmabuf 帧提交 plane，跳过 cairo/wl_shm。视频内容
    // 在硬件 plane 上，UI 无逐帧变化——不再 markDirty：否则 weston 被
    // 拉着 60fps 重绘提交，既耗 CPU 又可能与 plane 提交互相干扰
    if (drmActive_ && f.isDmabuf && submitDrmFrame(f)) {
        return;
    }

    // 时延配账严格配帧：上一帧的 frame callback 还没回来（frameCbPending_）
    // 就不动 pendingLat_——否则 done 回调到来时对的是新帧的账，数字全串
    if (cfg_.player.latencyTrace && f.ptsNs >= 0 && !frameCbPending_) {
        pendingLat_ = PendingLatency{};
        pendingLat_.ptsNs = f.ptsNs;
        pendingLat_.tReadUs = media_->takeReadTimeUs(f.ptsNs);
        pendingLat_.tPullUs = g_get_monotonic_time();
        pendingLat_.valid = true;
    }

    model_.markDirty();
    maybeRepaint();
}

void AndroidAutoApp::maybeRepaint() {
    if (model_.takeDirty() && model_.hasSize()) {
        repaint();
    }
}

void AndroidAutoApp::repaint() {
    const auto w = model_.windowWidth();
    const auto h = model_.windowHeight();

    ShmBuffer& buf = acquireFreeBuffer();
    if (buf.width() != w || buf.height() != h) {
        buf.destroy();
        if (!buf.create(wl_.shm(), w, h)) {
            log_.write("[buf] create %dx%d failed", static_cast<int>(w),
                       static_cast<int>(h));
            return;
        }
    }

    // 当前视频帧（data 在下一次 pull 前有效，主线程串行安全）
    VideoFrameView fv;
    if (currentFrame_.data != nullptr) {
        fv.data = currentFrame_.data;
        fv.width = currentFrame_.width;
        fv.height = currentFrame_.height;
        fv.stride = currentFrame_.stride;
    }

    cairo_t* cr = cairo_create(buf.cairoSurface());
    view_->paint(cr, model_, fv.data != nullptr ? &fv : nullptr);
    cairo_destroy(cr);

    // 时延·提交点：attach 后、commit 前取时刻；一次只挂一个 frame
    // callback（done 严格配帧，防串帧），pending 时整帧不追。
    if (pendingLat_.valid && !frameCbPending_) {
        pendingLat_.tCommitUs = g_get_monotonic_time();
        wl_callback* cb = wl_surface_frame(wl_.surface());
        wl_callback_add_listener(cb, &kFrameListener, this);
        frameCbPending_ = true;
    }

    wl_surface_attach(wl_.surface(), buf.wlBuffer(), 0, 0);
    wl_surface_damage_buffer(wl_.surface(), 0, 0, w, h);
    buf.setBusy(true);
    wl_surface_commit(wl_.surface());
    wl_.flush();
}

ShmBuffer& AndroidAutoApp::acquireFreeBuffer() {
    for (std::uint8_t i = 0; i < 2; ++i) {
        const auto idx = static_cast<std::uint8_t>(currentIdx_ ^ i);
        if (!buffers_[idx].busy()) {
            currentIdx_ = idx;
            return buffers_[idx];
        }
    }
    // 两块都被合成器占用：等 release 事件
    if (wl_.dispatch() >= 0 && !buffers_[currentIdx_].busy()) {
        return buffers_[currentIdx_];
    }
    // 极端情况（等不到事件）：直接复用，可能撕裂但不致挂死
    return buffers_[currentIdx_];
}

// ---------------------------------------------------------------------------
// 时延链路·上屏点 + 统计
// ---------------------------------------------------------------------------

const wl_callback_listener AndroidAutoApp::kFrameListener = {
    &AndroidAutoApp::onFrameDoneThunk,
};

void AndroidAutoApp::onFrameDoneThunk(void* data, wl_callback* cb,
                                      std::uint32_t /*time*/) {
    auto* self = static_cast<AndroidAutoApp*>(data);
    wl_callback_destroy(cb);
    self->frameCbPending_ = false;
    self->reportLatency();
}

void AndroidAutoApp::reportLatency() {
    if (!pendingLat_.valid) {
        return;
    }
    if (!firstScreenDone_) {
        firstScreenDone_ = true;
        log_.write("[startup] first frame on screen +%.1f ms",
                   (g_get_monotonic_time() - tStartUs_) / 1000.0);
    }
    const std::int64_t tScreenUs = g_get_monotonic_time();
    if (pendingLat_.tReadUs == 0) {
        // 读取点没抓到（头几帧/探针未命中）：跳过本帧，不污染统计
        pendingLat_.valid = false;
        return;
    }
    const double toPull =
        (pendingLat_.tPullUs - pendingLat_.tReadUs) / 1000.0;
    const double toCommit =
        (pendingLat_.tCommitUs - pendingLat_.tPullUs) / 1000.0;
    const double toScreen = (tScreenUs - pendingLat_.tCommitUs) / 1000.0;
    const double total = toPull + toCommit + toScreen;

    stReadPull_.add(toPull);
    stPullCommit_.add(toCommit);
    stCommitScreen_.add(toScreen);
    stTotal_.add(total);
    ++latFrames_;

    if (cfg_.player.latencyTrace && cfg_.player.latencyInterval > 0 &&
        latFrames_ % cfg_.player.latencyInterval == 0) {
        log_.write("[latency] pts=%.0fms | read->pull %.1f | pull->commit "
                   "%.1f | commit->screen %.1f | e2e %.1f (ms)",
                   pendingLat_.ptsNs / 1000000.0, toPull, toCommit, toScreen,
                   total);
    }
    pendingLat_.valid = false;
}

void AndroidAutoApp::logLatencySummary() {
    if (stTotal_.n == 0) {
        return;
    }
    const auto avg = [](const LatStats& s) { return s.sum / s.n; };
    log_.write("[latency-summary] frames=%llu | read->pull avg %.1f "
               "min %.1f max %.1f | pull->commit avg %.1f | commit->screen "
               "avg %.1f | e2e avg %.1f min %.1f max %.1f (ms)",
               static_cast<unsigned long long>(stTotal_.n),
               avg(stReadPull_), stReadPull_.min, stReadPull_.max,
               avg(stPullCommit_), avg(stCommitScreen_), avg(stTotal_),
               stTotal_.min, stTotal_.max);
}

} // namespace aa
