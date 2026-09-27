# AndroidAuto

基于 ivi-shell（Weston 13 / x9m_ms PTG6.1）的 Wayland 客户端应用。
功能：launcher 图标点击启动 → 应用窗口（标题栏 + 右上角关闭按钮 + 播放窗口）
→ 音视频播放（两条独立 GStreamer 管线）+ 端到端时延统计。

## 1. 架构（MVC）

```
Controller (src/app)     InputController   输入事件 → 语义动作（关闭/埋点）
                         AndroidAutoApp    组合根 + glib 主循环 + 时延统计
                         VideoPlayer       视频管线（playbin 静音，appsink BGRx）
                         AudioPlayer       音频管线（uridecodebin，独立输入源）
Model      (src/model)   AppConfig         ini 配置解析（[app]/[window]/[player]/[ui]）
                         AppModel          运行时状态（尺寸/视频尺寸/dirty/退出）
View       (src/view)    AppView           cairo 绘制（视频帧零拷贝绘入播放区）
                         Layout            纯几何计算（播放区矩形/宽高比自适应）
基础设施    (src/wayland) WlClient          wayland 连接/ivi_surface/输入/hmi-controller
                         ShmBuffer         RAII shm 双缓冲
           (src/util)    Log               RAII 文件日志
```

事件流（全部汇到 glib 主循环，主线程是唯一碰 wayland 的线程）：
- wayland socket fd → dispatch（configure/输入/buffer release）
- GStreamer 流线程 new-sample → 原子标记 + idle 投递 → 主线程拉帧 → 重绘
- 总线 watch ×2（预卷完成自动 PLAYING、EOS 循环、错误日志）
- 定时器：SIGTERM/SIGINT 优雅退出、音视频 PTS 漂移监测

时延统计链路（对齐参考实现 ivi_video_player）：读取点（demuxer 探针按 PTS）
→ 拉出点（pullFrame）→ 提交点（attach/commit）→ 上屏点（wl_surface_frame
done），`[latency]` 逐段打印 + 退出 `[latency-summary]` 汇总。

## 2. 依赖

| 依赖 | 版本 | 来源 |
|---|---|---|
| 交叉工具链 aarch64 gcc/g++ | 13.2 | x9sp_wayland SDK |
| wayland-client | 1.22.0 | SDK 目标 sysroot（含头文件/.pc） |
| cairo | 1.18.0 | 同上 |
| glib-2.0 / gio-unix-2.0 | 2.78 | 同上（主循环/信号） |
| gstreamer-1.0 / app / video | 1.22.11 | 同上（播放） |
| wayland-scanner（构建机原生 x86_64） | 1.22.0 | SDK 原生 sysroot |

协议 glue（`ivi-application` + `ivi-hmi-controller`）由 Makefile 在构建时
从 `protocol/*.xml` 自动生成到 `obj/protocol/`，无需手工操作。

## 2.1 板级/环境约束（开发必读）

1. **VPU 只支持一路 h265 硬解**：AudioPlayer 的 uridecodebin 用
   `autoplug-continue` 拒绝视频轨插解码器（第二路 omxh265dec enable 失败
   会拖死整条音频管线，实测）。
2. **无 AAC 解码器**（gst-inspect 仅 aacparse）：mp4 的 AAC 音轨解不了，
   音频通路用 FLAC 验证（flacdec 可用）；BSP 补插件后可换回。
3. **音频 caps 钉死 S16LE/2ch/48kHz**：TLV320AIC23 唯一验证格式（asound.conf
   已把 default 钉到 card 1 hw）。
4. **视频输出 BGRx**：weston pixman 软合成经 wl_shm 的要求，硬解 NV12 必须
   videoconvert 显式转换（参考 ivi_app TEST.md §6/§8）。
5. **VPU/OMX 状态切换不可同步阻塞等待**：总线消息驱动（参考 TEST.md §7）。

## 2.2 布局控制路径（fullscreen_layout 当前阻塞的说明）

需求：应用窗口占领 launcher 栏上方全部区域（application layer = 1920×650）。
现状：hmi-controller 的 tiling 布局把所有应用分格（480×325），客户端无法
自行扩大。已验证的可行路径（需 compositor/BSP 侧配合，**应用侧代码已就绪**：
`WlClient::switchLayout()` + ivi-hmi-controller 协议 glue，配置
`fullscreen_layout=1` 即启用启动切 full_screen / 退出恢复 tiling）：

1. 定制版 hmi-controller.so 的 `bind_hmi_controller` 有权限校验：仅允许
   weston-ivi-shell-user-interface 绑定，其余客户端 bind 即 fatal 协议错误
   （`hmi-controller failed: permission denied`，实测+反汇编确认，bind 处
   `b.eq` 单点跳转）。厂商放行（去校验或按 ivi id 白名单）即可。
2. ivi_wm / ILM 控制面（ivi_controller 项目路线）：当前设备未广播 ivi_wm
   全局、无 libilm 库，不可用。
3. weston 16 上游 hmi-controller 的 desktop surface 路径同样走 tiling
   switch_mode，无区别待遇。

## 3. 上手开发

```bash
# 1) 加载交叉编译环境（必须，提供 $CC/$CXX/pkg-config 指向 aarch64 sysroot）
. /home/admin0412/x9sp_wayland/environment-setup-cortexa55-sdrv-linux

# 2) 构建
make                # 产物 bin/AndroidAuto
make check          # 编译 + file/readelf 产物检查
make protocol       # 仅生成协议 glue
make clean          # 清除 bin/ obj/

# 3) 本地静态检查（可选，快速发现链接错误）
aarch64-sdrv-linux-readelf -d bin/AndroidAuto | grep NEEDED
# 预期 NEEDED: libwayland-client.so.0 libcairo.so.2 libstdc++.so.6
#             libgcc_s.so.1 libm.so.6 libc.so.6
```

## 4. 运行时配置（config/androidauto.ini）

加载顺序（相对可执行文件目录）：
`<exe目录>/androidauto.ini` → `<exe目录>/../config/androidauto.ini` → `./androidauto.ini`。
找不到则使用代码内置默认值（界面仍可用）。

| 段 | 键 | 说明 |
|---|---|---|
| [app] | surface_id | ivi surface id（3010，application 段） |
|      | log_path | 日志文件（空 = stderr） |
| [window] | title | 标题栏文字 |
|          | default_width/height | 首帧兜底尺寸（configure 后自适应） |
|          | logo_path | 标题 logo PNG，相对路径基于 ini 目录 |
| [player] | enabled | 播放总开关（0 = 只显示占位窗口） |
|          | video_uri / audio_uri | 两条管线各自的输入（file:// URI 或绝对路径；audio_uri 空 = 跟 video_uri） |
|          | loop | EOS 循环播放 |
|          | fullscreen_layout / restore_layout | 启动切 full_screen / 退出恢复 tiling（需 compositor 侧放行，见 §2.2） |
|          | latency_trace / latency_interval | 时延统计开关与打印间隔（帧） |
|          | drift_interval_sec | 音视频 PTS 漂移监测周期（秒，0 = 关） |
| [ui] | title_bar_height / close_button_size / close_button_margin | 标题栏与关闭按钮 |
|      | video_margin | 播放区相对应用窗口上下左右边距（px） |
|      | video_border_width / video_border_radius | 播放窗口边框 |
|      | video_aspect_w / video_aspect_h | 播放窗口宽高比（0/0 = 跟随视频实际比例） |
|      | font_size / placeholder_text | 字体与占位文案 |
|      | color_* | 配色，格式 `0xRRGGBBAA` |

资源统一放 `res/`（icons/ 图标，shaders/ 预留），由 ini 引用；
**代码内无任何 UI 尺寸、颜色、路径硬编码**。

## 5. 部署到设备

```bash
# 一键部署（只推运行时必需文件：二进制/配置/资源/weston.ini，不含源码）
./push.sh                 # 推送 + 重启 weston
./push.sh --no-restart    # 仅推送（改 UI 参数后连 weston 都不用重启，重开 app 即可）
./push.sh <serial>        # 多设备时指定 adb serial
```

脚本做的事：推送 `bin/AndroidAuto`、`config/androidauto.ini`、
`res/icons/androidauto.png` 到 `/data/AndroidAuto/`，推送 `../weston.ini`
（含 icon-id=4007 launcher 入口）到 `/data/weston.ini`，然后执行
`/data/start_ivi.sh` 重启 weston。

ini 加载路径验证：可执行文件在 `/data/AndroidAuto/bin/`，配置位于
`/data/AndroidAuto/config/androidauto.ini`，由搜索顺序第 2 条命中。

运行验证：
```bash
adb shell "pgrep AndroidAuto && echo RUNNING"
adb shell "cat /tmp/androidauto.log"        # 触点坐标/生命周期日志
adb shell "journalctl -u weston --since -2min | tail -20"
```

ini 加载路径验证：可执行文件在 `/data/AndroidAuto/bin/`，配置位于
`/data/AndroidAuto/config/androidauto.ini`，由搜索顺序第 2 条命中。

## 6. 测试

见 `TEST_PLAN.md`（用例编号 TC A/B/C/D 系列，验收标准）。

## 7. 目录结构

```
AndroidAuto/
├── Makefile  .gitignore  README.md  DEV_PLAN.md  TEST_PLAN.md
├── config/androidauto.ini      # 运行时配置
├── protocol/ivi-application.xml # ivi_application 协议定义
├── res/icons/  res/shaders/     # 资源（图标 / 预留 GL）
├── src/
│   ├── main.cpp
│   ├── app/      (Controller)  AndroidAutoApp  InputController
│   ├── model/    (Model)        AppConfig  AppModel
│   ├── view/     (View)         AppView  Layout
│   ├── wayland/  (基础设施)     WlClient  ShmBuffer
│   └── util/                    Log
├── bin/  # 产物（gitignored）
└── obj/  # 产物 + 协议 glue（gitignored）
```

## 8. 故障排查

| 现象 | 排查 |
|---|---|
| 启动报 `wl_display_connect failed` | `XDG_RUNTIME_DIR`/`WAYLAND_DISPLAY` 未设置，或 weston 未运行 |
| 报 `missing globals ... ivi_application` | weston 没跑 ivi-shell，检查 `shell=ivi-shell.so`，跑 `/data/start_ivi.sh` |
| app 启动但黑屏 | 看 `/tmp/androidauto.log` 是否有 configure；确认首帧尺寸（default_width/height）与屏幕匹配 |
| 点击关闭无效 | 看 log 的 `[touch] x= y=`，若坐标系统性偏移，用 weston.ini `[libinput]` 校准触控 |
| 界面元素错位 | 调整 `config/androidauto.ini` 的 [ui] 度量，无需重编译 |
