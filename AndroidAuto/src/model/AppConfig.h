#pragma once

#include <cstdint>
#include <string>

#include "media/MediaTypes.h"

namespace aa {

/// 播放器配置（[player] 段）。统一媒体管线（screencast-avsync-design）：
/// 音视频共用同一 GstPipeline（固定系统时钟），输入统一为媒体包接口
/// （文件源由 FilePacketFeeder 拆包；投屏协议层将来直连同一接口）。
struct PlayerConfig {
    bool        enabled          = true;   ///< 是否启用播放（false 回到占位窗口）
    bool        audioEnable      = true;   ///< 音频会话总开关（0 = 纯视频；调试/无音频投屏用）
    std::string videoUri;                  ///< 视频源文件（兼作音频源，见 audio_uri）
    std::string audioUri;                  ///< 独立音频源（空 = 跟 video_uri）
    bool        loop             = true;   ///< EOS 后循环（PTS 偏移累加）
    bool        fullscreenLayout = false;  ///< 启动切 full_screen（需 compositor 侧放行，见 README）
    bool        restoreLayout    = true;   ///< 退出时切回 tiling
    bool        latencyTrace     = true;   ///< 逐帧时延统计（[latency] 日志行）
    std::uint32_t latencyInterval = 60;    ///< 每隔多少帧打印一行（0 = 只打总结）
    std::uint32_t driftIntervalSec = 2;    ///< 音画纠偏周期（秒，0 = 关）
    Transport   transport        = Transport::Usb; ///< USB/WiFi profile（queue 深度/卡顿阈值）
    std::uint32_t queueMaxMs     = 0;      ///< 解码前 queue 深度覆盖（0 = profile 值）
    std::uint32_t stallThresholdMs = 0;    ///< 卡顿阈值覆盖（0 = profile 值）
    bool        stallDetect      = false;  ///< 卡顿检测+IDR 恢复（投屏用；文件源须关，见 MediaPlayer.h）
    // ---- DRM 直扫渲染（混合路线 C，见 2026-09-18-drm-plane-action-plan.md）----
    bool        drmRender        = false;  ///< render_path=drm：视频帧经空闲 overlay plane 直扫（否则 wl_shm+cairo）
    std::string drmDevice        = "/dev/dri/card0"; ///< DRM 设备节点
    std::uint32_t drmPlaneId     = 0;      ///< 0=自动挑选空闲 plane；非零=强制指定（排障用）
    std::int32_t drmX            = 0;      ///< 视频目标矩形（屏幕绝对坐标，需与实际 surface 位置校准）
    std::int32_t drmY            = 0;
    std::int32_t drmW            = 1920;   ///< 默认 = application layer 全宽
    std::int32_t drmH            = 650;    ///< 默认 = 屏高 - launcher 栏
    std::int32_t evdevScreenW    = 1920;   ///< evdev 鼠标坐标累积边界（屏幕分辨率）
    std::int32_t evdevScreenH    = 720;
    /// VPU dmabuf 的 DRM modifier。2026-09-18 查证：BSP OMX 源码
    /// （omx_vpudec_component.c wtlEnable 恒真 + gstomxvideodec.c 仅
    /// caps 带 IFBC feature 才切 tiled 输出，本管线未带该 feature）+
    /// `modetest` 实测 plane 43 NV12 IN_FORMATS 均证实 use-dmabuf 输出
    /// 就是硬件 WTL 后的真 LINEAR NV12，并非 Semidrive WAVE tile 布局；
    /// 0 = LINEAR 才是与实际数据匹配的值（22/31 等 WAVE_32X8_* 值曾是
    /// 错误假设，会被 plane 每帧拒绝，靠回退路径侥幸显示）。
    std::uint64_t drmModifier    = 0;
};

/// UI 度量与配色。默认值仅在 ini 缺失时兜底；
/// 正常运行时全部字段来自 config/androidauto.ini（禁止代码内硬编码 UI）。
struct UiStyle {
    std::uint16_t titleBarHeight   = 120;
    std::uint16_t closeButtonSize  = 96;
    std::uint16_t closeButtonMargin = 12;
    std::uint32_t videoMargin      = 20;
    std::uint16_t videoBorderWidth = 4;
    std::uint16_t videoBorderRadius = 12;
    std::uint16_t videoAspectW     = 0;   ///< 0 = 跟随视频实际宽高比
    std::uint16_t videoAspectH     = 0;
    std::uint16_t fontSize         = 28;
    std::uint32_t colorBackground  = 0xFF1A1D26u;
    std::uint32_t colorTitleBar    = 0xFF10141Cu;
    std::uint32_t colorBorder      = 0xFF2D8CFFu;
    std::uint32_t colorVideoFill   = 0xFF0A0E14u;
    std::uint32_t colorCloseGlyph  = 0xFFE8E8E8u;
    std::uint32_t colorText        = 0xFF9AA4B2u;
    std::string   placeholderText  = "Video Window (no content)";
};

/// 全局配置（surface id、窗口兜底尺寸、资源路径）
struct Config {
    std::uint32_t surfaceId     = 3010;
    std::string   logPath;
    std::string   title         = "AndroidAuto";
    std::int32_t  defaultWidth  = 1920;
    std::int32_t  defaultHeight = 1000;
    std::string   logoPath;      // 已解析为绝对路径（相对路径基于 ini 目录）
    PlayerConfig  player;
    UiStyle       ui;
};

/// ini 加载器。搜索顺序：
///   <exeDir>/androidauto.ini → <exeDir>/../config/androidauto.ini → ./androidauto.ini
class AppConfig {
public:
    /// 找到并读取返回 true（个别键值非法则跳过并计入 errCount）；
    /// 一个都没找到返回 false（调用方使用 Config 默认值）。
    static bool load(const std::string& exeDir,
                     Config& out,
                     std::string& iniPathOut,
                     std::size_t& errCount);
};

} // namespace aa
