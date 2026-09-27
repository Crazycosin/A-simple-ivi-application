# AndroidAuto 项目 Context Checkpoint

> **用途**：新开 AI 会话时，让助手快速恢复进度上下文，从此处继续辅助开发。
> **更新约定**：每完成一个阶段/修复一个重要问题，更新本文档的「当前状态」与「待办」。
>
> | | |
> |---|---|
> | 更新时间 | 2026-09-18（追加：CursorOverlay 鼠标光标 plane §9.9、光标移动节流 §9.10、音频权限修复 §9.11） |
> | 项目阶段 | v1.3 DRM 直扫混合路线（render_path=drm）实现完成，视频 dmabuf 零拷贝上屏 + 退出恢复 + evdev 坐标通道就绪，root 与 weston 用户（launcher 同身份）均验证通过；**本次连续修了三个问题：drm_modifier 错误注入（§9.4）、视频解码器致命错误后无自愈（§9.5）、chroma plane offset 从未赋值导致绿幕（§9.6，最可能是"画面不纯净"的真正主因）。三者日志层面均已验证，像素级视觉最终确认待人工回归** |
> | git 最新 | `2db4cd5`（checkpoint 存档提交；v1.1/v1.2/v1.3 改动**待提交**） |

---

## 0. 新会话冷启动指南（AI 先读这个）

1. **读文档顺序**：本文件 → `README.md`（构建/部署/配置/板级约束）→ `2026-09-17-media-pipeline-appsrc-live-deadlock.md`（v1.2 死锁根因）→ `2026-09-18-dmabuf-zero-copy-route.md`（dmabuf 零拷贝路线调研 + 方案对比总表）→ `2026-09-18-drm-plane-action-plan.md`（DRM 直扫路线行动书）→ 需要时 `2026-09-16-CHECKPOINT.md` / `2026-09-17-CHECKPOINT.md`（历史）
2. **构建**：`. /home/admin0412/x9sp_wayland/environment-setup-cortexa55-sdrv-linux && make`（项目根目录）
3. **部署**：`./push.sh`（推运行时文件 + 重启 weston）或 `./push.sh --no-restart`
4. **设备**：adb（root），x9m_ms，Weston 13 定制 ivi-shell；**媒体运行需 x9hp 环境**（AAC 软解在 `/opt/x9hp/lib/gstreamer-1.0/libgstlibav.so`），入口 `/data/AndroidAuto/start.sh`（source /opt/x9hp/env.sh 后 exec 二进制）
5. 严禁凭记忆写 wayland 回调签名——**先查 sysroot 头文件**；严禁改动第三方库/设备系统文件
6. 媒体架构问题先读 `src/media/MediaPlayer.h` 头注释（设计文档 § 对应关系都在里面）；DRM 直扫读 `src/view/DrmView.h`（plane 挑选安全规则）、evdev 输入读 `src/input/EvdevReader.h`

## 1. 项目是什么

- **AndroidAuto**：ivi-shell wayland 客户端应用（C++17 / MVC / cairo / RAII / glib 主循环 / GStreamer）
- **v1.2 媒体架构**（screencast-avsync-design.md 落地）：
  - **单管线 + 固定 GstSystemClock**：`MediaPlayer` 持有 GstPipeline，音视频两个 bin 动态挂拆，时钟不随分支增删切换
  - **统一媒体包接口**：`pushVideoPacket/pushAudioPacket`（任意线程，block 反压不丢包）——文件与投屏同一路径
  - **FilePacketFeeder**：filesrc!parsebin 拆包，fakesink **sync=TRUE 1x 重放**（给文件源造实时节奏），EOS 循环 = **整管线重建 + ptsOffset 累加**
  - **显式解码链**：视频 `queue→h265parse→omxh265dec(硬解)→videoconvert(BGRx)→appsink(sync=FALSE)`；音频 `queue→aacparse→avdec_aac(软解)→convert→S16LE/2ch/48k→alsasink(sync=TRUE)`；解码器按首包 caps 选定（h264/h265，硬解缺落 libav 软解）
  - **音画纠偏**：仅两路并存时运行，2s 周期，±20ms 死区，小步长只调视频 ts-offset
  - **卡顿检测+IDR 恢复**（投屏机制）：`stall_detect` 开关，**文件源必须关**
- **v1.3 DRM 直扫混合路线**（2026-09-18 新增，`render_path=drm`）：
  - **架构**：weston 继续当 launcher/UI/输入的家；视频绕开 weston，经**空闲 DRM overlay plane 直扫**（dmabuf 零拷贝，不 videoconvert 不 wl_shm）
  - **视频链双模式**：`drmOutput=true` 时 `omxh265dec use-dmabuf=true` → `appsink(memory:DMABuf)` 取 fd（无 videoconvert/capsfilter）；`drmOutput=false` 走原 BGRx shm 路径（保底，DrmView 创建失败/软解兜底自动回退）
  - **DrmView**（src/view/）：`AddFB2WithModifiers` → `drmModeSetPlane` 提交空闲 YUV overlay plane；**不 modeset、不碰 primary/cursor**，退出只关自己 plane
  - **EvdevReader**（src/input/）：evdev 多读者直读鼠标（与 weston 并行不抢夺），视频区命中判断 + 屏幕/视频像素坐标上报（协议层注入通道预留 TODO）
  - **退出恢复**：`teardown` 关 plane 后 commit 一帧 damage 主动触发 weston repaint（免"动鼠标才恢复"）
- 目录：`/home/admin0412/windows_share/x9_linux/data/AndroidAuto/`

## 2. 关键环境事实（host）

| 项 | 值 |
|---|---|
| SDK | `/home/admin0412/x9sp_wayland/`（wayland 1.22 + cairo 1.18 + glib/gio + gstreamer 1.22 + **libdrm + gstreamer-allocators 开发包齐全**） |
| 备用 SDK | `/home/admin0412/x9sp/`（缺 wayland 开发包，勿用） |
| 参考实现 | `/home/admin0412/projects/ivi_app/`（ivi_video_player 等）；DRM 直扫参考 `/home/admin0412/windows_share/x9_linux/display-test/`、EGL import 参考 `/home/admin0412/windows_share/x9_linux/egl-demo/` |
| 设计文档 | 项目内 `screencast-avsync-design.md`（媒体架构）+ `2026-09-18-drm-plane-action-plan.md`（DRM 行动书）+ `2026-09-18-dmabuf-zero-copy-route.md`（路线调研） |
| 源码编辑 | **勿用 Windows 编辑器保存**——会把文件转 CRLF 导致 patch 失配（用 dos2unix/sed 修复） |

## 3. 设备侧状态（x9m_ms）

| 项 | 状态 |
|---|---|
| weston 13 + ivi-shell | ✅ 运行中（start_ivi.sh bind-mount hmi-controller 等） |
| 播放入口 | `/data/AndroidAuto/start.sh`（launcher 图标也指这里；**必须经它启动**） |
| 视频 | ✅ omxh265dec VPU 硬解（`/dev/vpucoda /dev/vpuwave`）；**use-dmabuf=true 输出 dmabuf**（实测 315fps 零拷贝，见 `2026-09-17-h265-dmabuf-performance-test.md`） |
| 音频 | ✅ avdec_aac 软解（/opt/x9hp）→ ALSA card1 TLV320AIC23；weston 用户已加入 audio 组（§9.11，仅运行时生效，重刷机需重做/落 BSP），实际出声待耳朵确认 |
| DRM plane | plane 43 = 空闲 Overlay（type=0，格式匹配 NV12）；weston 用 primary plane；非 master 进程 setplane 共存**已验证** |
| 验证数据 | DRM 路径首帧 `first dmabuf frame decoded (1920x960, NV12, planes=2)`，`v_queue 0us` 消费及时 |

## 4. 设备/工具怪癖清单（新会话必读）

1. wayland 1.22 SDK 回调签名与上游不同——写 thunk 前查 sysroot 头文件
2. libwayland：listener NULL 回调直接 abort；WlClient 已全事件 no-op
3. 定 hmi-controller：icon 路径失效 → add_launchers assert → **weston ABRT**
4. BusyBox：无 timeout；ps 不可信（扫 /proc）；**kill 按 comm 精确匹配**（cmdline 通配会杀到 shell 自身）；`head -n N` 带参数
5. 无 RTC：日志时间戳跨 boot 重叠，判断归属用 uptime
6. rootfs 只读；/tmp tmpfs；持久文件放 /data
7. 双屏 1920×720；configure 给 tiling 格子尺寸（480×325）
8. **VPU 只支持一路 h265 硬解**（多路 omx enable 失败）
9. rootfs 无 AAC 解码器（仅 aacparse）；软解在 /opt/x9hp（LD_LIBRARY_PATH/GST_PLUGIN_PATH 由 start.sh 注入，运行时 setenv 无效）
10. **playbin2(GStreamer 1.22) flags 关音频后仍激活真实 audio sink**（抢 ALSA）——已弃用 playbin
11. **appsrc is-live=TRUE 与动态挂载 bin 的 preroll 互锁**（静默死锁，详见 `2026-09-17-media-pipeline-appsrc-live-deadlock.md`）
12. hmi-controller bind 权限校验：仅 weston-ivi-shell-user-interface 可绑 ivi_hmi_controller，其他客户端 bind 即 fatal 协议错误；full_screen 路线等厂商放行
13. parsebin/qtdemux 对 FLUSH seek 不可靠——循环用整管线重建
14. gst-launch 对照组里 demux pad 与 sink 之间必须加 queue，否则对照组自己假卡死
15. **bellagio OMX core 按 `$HOME` 找组件注册表 `.omxregister`**：launcher 以 weston 用户跑（HOME=/home/weston 只读、无该文件）→ core init 失败（连 /dev/vpucoda 都不试）；root 跑时 HOME 空 fallback `/tmp/.omxregister` 成功。**修复：start.sh `export HOME=/tmp`**（§9 #1）
16. **错误态音频 bin 挂进管线会拖死视频 preroll**（v_queue 顶满无帧，事后拆除也救不回）——修复：挂 bin 前 **ALSA 预探测**（alsasink 单独试 READY，失败不挂）+ `audioDead_` 防重建循环（§9 #2）
17. **weston 用户（launcher 启动身份）不在 `audio` 组**（`/dev/snd` root:audio 660）→ ALSA Permission denied。应用侧已能跳过音频保视频；正式音频需设备侧把 weston 用户加 audio 组（rootfs 只读禁改）
18. **drm_plane_type 枚举**：Overlay=0 / Primary=1 / **Cursor=2**（不是 3）；primary/cursor 一律不碰
19. 触摸天生收不到：内核 `CONFIG_TOUCHSCREEN_SEMIDRIVE` 未编译，gt9271 驱动不在（换内核才能修）；鼠标可用（CONFIG_USB_HID=y）

## 5. 关键文件地图

```
src/
├── app/AndroidAutoApp.*    组合根：glib 主循环/wayland fd/信号/时延统计/装配；DRM 分支 + evdev 回调 + 退出恢复
├── app/InputController.*   输入→关闭等语义
├── input/EvdevReader.*     ★v1.3 新增：evdev 多读者直读鼠标，坐标累积+钳制+按下沿回调
├── media/MediaTypes.h      Transport profile/VideoFrame（v1.3 扩展 dmabuf 描述：fd/offset/pitch/fourcc/modifier）
├── media/MediaPlayer.*     ★统一管线；v1.3 加 Options.drmOutput + 视频链双模式 + dmabuf 帧输出 + ALSA 预探测
├── media/FilePacketFeeder.* ★文件→包桥：parsebin 拆包+1x 重放+循环重建
├── model/AppConfig.*       ini 解析（[app]/[window]/[player]/[ui]；v1.3 加 render_path/drm_*/evdev_*）
├── model/AppModel.h        窗口/视频尺寸/dirty
├── view/AppView.*          cairo 绘制（BGRx 零拷贝绘帧；shm 保底路径）
├── view/DrmView.*          ★v1.3 新增：DRM plane 直扫（AddFB2WithModifiers→setplane 空闲 overlay），固定接口 setImageData/setViewport/clear/hitTest
├── view/Layout.*           几何（videoWindow 宽高比自适应）
├── wayland/WlClient.*      连接/ivi_surface/输入/hmi-controller(按需绑)
├── wayland/ShmBuffer.h     wl_shm 双缓冲
└── util/Log.h              RAII 日志
config/androidauto.ini      全参数（player 段见 §6，v1.3 加 DRM 段）
start.sh                    设备启动包装（x9hp 环境 + HOME=/tmp）
push.sh                     部署（含 start.sh）
protocol/                   ivi-application + ivi-hmi-controller XML
```

## 6. [player] 配置速查（config/androidauto.ini）

| 键 | 当前值 | 说明 |
|---|---|---|
| video_uri | h265_aac_1920x960_60fps.mp4 | 文件源（feeder 拆包出两路） |
| audio_uri | 空 | 空 = 同 video_uri；独立源须 AAC |
| audio_enable | 1 | 音频会话总开关（0 = 纯视频；weston 用户无 audio 组时自动跳过） |
| loop | 1 | EOS 循环（管线重建+PTS 偏移） |
| transport | usb | usb=queue 100ms/卡顿 60ms；wifi=500ms/300ms |
| stall_detect | 0 | **文件源必须 0**（投屏网络机制） |
| drift_interval_sec | 2 | 音画纠偏周期 |
| latency_trace / latency_interval | 1 / 60 | 时延统计 |
| fullscreen_layout | 0 | 需 compositor 侧放行（§4.12） |
| **render_path** | shm（默认）/ drm | drm = 视频经空闲 overlay plane 直扫（dmabuf 零拷贝）；shm = 原 wl_shm+cairo（DrmView 创建失败也自动回退） |
| **drm_device** | /dev/dri/card0 | DRM 设备节点 |
| **drm_plane_id** | 0 | 0 = 自动挑选空闲 plane（跳过 primary/cursor/占用）；非零 = 强制指定（排障） |
| **drm_viewport** | 0,0,1920,650 | 视频目标矩形（屏幕绝对坐标，需与实际 surface 位置校准） |
| **evdev_screen_w / evdev_screen_h** | 1920 / 720 | evdev 鼠标坐标累积边界（屏幕分辨率） |

## 7. 待办事项（按优先级）

0. **画面不纯净：像素级视觉确认（最高优先级）**：目前已连续定位并修了三个
   问题——`drm_modifier` 错误注入（§9.4）、视频解码器致命错误后管线永久卡死
   不自愈（§9.5）、**chroma plane offset 从未赋值导致绿幕**（§9.6，用户反馈
   "一层绿幕"后定位，最可能是真正主因：NV12 的 UV 平面 offset 一直是 0，
   显示硬件把 Y 平面数据当 UV 采样）。三者日志层面都已验证（§9.6 首帧日志
   `pitch0=1920, offset1=1843200` 精确等于 luma 平面大小），但**仍需下次
   播放时人工肉眼确认绿幕是否真的消失**。若肉眼确认仍有残留色偏/花屏，
   下一个该查的候选是双 fb 轮换与 VPU 输出 buffer 池生命周期竞争（行动书
   R3 风险），不要再退回猜 modifier 这条老路（已被 §9.4 证伪）
1. **鼠标实测 launcher 点击启动 → 播放 → 点击视频区（看 `[evdev] video-area click` 坐标日志）→ ✕/信号退出 → launcher 立即恢复**（用户手动）。退出/恢复这半段已用**真实 launcher 点击**（非 adb 命令行绕过）验证通过，含一次 vendor OMX 关闭卡死的复现 + 看门狗强杀 + 立即重新拉起成功（§9.7/§9.8）；**尚未验证的只剩"点击视频区坐标上报"这一小半**
2. **drm_viewport 坐标校准**：确认 `0,0,1920,650` 与实际 surface 位置/播放区对齐（依赖待办 1 的实测反馈调整）
3. **音频权限**：已在设备侧把 weston 用户加入 `audio` 组并重启 weston 生效（见 §9.11），但只是运行时改动、重刷机会丢——长期还需落进 BSP 用户/组配置持久化；**实际出声是否正常仍待用户下次播放耳朵确认**
4. **投屏协议层接入**：MediaPlayer 包接口已就绪（push*Packet/setCaps/start*Session）；evdev 坐标上报通道已留 TODO（`onEvdevPointer` 里视频像素坐标归一化就绪），协议层接入时对接触摸/按键注入 + 开 stall_detect + IDR 请求通道
5. **git 提交**（用户确认后；建议拆笔：v1.1 双 playbin / v1.2 统一管线 / v1.3 DRM 直扫路线）
6. wayland 路线的 viewporter v3 bind bug（BSP 修 `libgstwayland`，作为"weston 内 dmabuf"路线备用，见 `2026-09-17-waylandsink-bsp-retest.md`）
7. full_screen 布局放行（BSP/厂商：hmi-controller bind 校验放行，材料在 README §2.2）
8. 触摸驱动（BSP 范围，CONFIG_TOUCHSCREEN_SEMIDRIVE=y）；修好后 EvdevReader 触摸事件结构预留可直接接
9. D-2 长时间内存复测（v1.3 架构下 5 分钟采样确认封顶行为）

## 8. git 历史

```
2db4cd5 CHECKPOINT: 上下文存档（新会话冷启动指南）  ← v1.2 末次提交
ebbb3de 排查沉淀：ivi 集成期五故障复盘
a768436 修复秒退二连：WAYLAND_SOCKET/WAYLAND_DISPLAY 环境 + listener NULL abort
（工作区：v1.1 双 playbin + v1.2 统一管线重构 + v1.3 DRM 直扫混合路线
 （DrmView/EvdevReader 新增 + MediaPlayer 双模式 + start.sh HOME 修复），
 待提交；多份排查/路线文档已落盘）
```

## 9. 本次会话（v1.3）修复/新增的问题速览

### 9.1 排查修复的根因（连环三连）

| # | 问题 | 修复 | 证据 |
|---|---|---|---|
| 1 | **bellagio OMX core 按 `$HOME` 找注册表**：weston 用户（launcher 身份）HOME=/home/weston 只读无 `.omxregister` → omx core init 失败（连 vpucoda 都不试）；root HOME 空 fallback `/tmp/.omxregister` 成功 | start.sh `export HOME=/tmp` | root 成功 vs weston 失败的 strace 对比（vpucoda 尝试 3 次 vs 0 次） |
| 2 | **错误态音频 bin 拖死视频 preroll**：weston 用户不在 audio 组 → alsasink open Permission denied → 错误态 audio bin 挂进管线卡住整条 preroll（v_queue 顶满无帧，事后拆链也救不回） | 挂 bin 前 **ALSA 预探测**（alsasink 单独试 READY，失败不挂）+ 运行中错误拆链兜底 + `audioDead_` 防重建循环 + `audio_enable` 配置项 | root 全通 vs weston 堵 v_queue；禁音频后 weston 用户视频全通 |
| 3 | plane type 枚举笔误（Cursor=2 非 3） | 修正 Overlay=0/Primary=1/Cursor=2；primary/cursor 一律不碰 | plane 43 实测 type=0（Overlay），澄清非 primary |

### 9.2 新增能力（v1.3 DRM 直扫混合路线）

| 模块 | 内容 |
|---|---|
| `DrmView` | plane 直扫：AddFB2WithModifiers（透传 modifier）→ setplane 空闲 YUV overlay；选 plane 规则（format + possible_crtcs + type 过滤 + FB_ID/CRTC_ID 占用检查）；双 fb 轮换；退出只关自己 plane |
| `EvdevReader` | evdev 多读者直读（与 weston 共存），鼠标相对坐标累积+钳制+按下沿回调 |
| `MediaPlayer` 双模式 | `Options.drmOutput` 切视频链；dmabuf 帧输出（GstVideoMeta 透传 fd/offset/pitch，GST 格式→DRM fourcc）；prevSample 保一拍防 pool 复写 |
| `VideoFrame` 扩展 | 新增 isDmabuf/fd[4]/offset[4]/pitch[4]/numPlanes/fourcc/modifier（原字段不动，向后兼容） |
| `AndroidAutoApp` 装配 | DRM 分支 + `onEvdevPointer` 坐标上报（视频像素坐标归一化）+ teardown 关 plane + commit damage 主动唤醒 weston |

### 9.3 验证结论

| 路径 | 视频 dmabuf | 音频 | 退出恢复 |
|---|---|---|---|
| root 命令行 | ✅ first dmabuf frame (NV12 planes=2) | ✅ ALSA 正常 | ✅ plane cleared + repaint |
| weston 用户（= launcher 身份） | ✅ 同上 | ⚠️ 预探测失败自动跳过（无声） | ✅ 同上 |

详细调研与方案对比见 `2026-09-18-dmabuf-zero-copy-route.md`，行动书见 `2026-09-18-drm-plane-action-plan.md`。

### 9.4 追加排查：画面不纯净根因（drm_modifier 错误注入）

**背景**：§9.3 验证的是"有画面 + 不丢帧"，从未做过像素级视觉核对。代码里一直
留着一条自我怀疑的注释（`MediaPlayer.cpp` 原 850 行附近）：怀疑 h265/wave412
的 dmabuf 输出是 Semidrive WAVE_32X8 tiled 布局，`drm_modifier` 按此猜测注入
非零值（22=WAVE_32X8_TILE，排障试过 31=WAVE_32X8_FBDC）。本次追查该猜测本身。

**排查手段与证据链**：

1. `adb shell` 抓 `androidauto.log`：**每一帧** `drmModeAddFB2WithModifiers`
   都以 `EINVAL` 被拒（"rejected: Invalid argument (retry linear)" 刷屏），
   22/31 两个值都是如此——回退路径（隐式 LINEAR 的 `drmModeAddFB2`）才是
   实际一直在用的路径；
2. `modetest -M semidrive -p` 实测 plane 43 的 NV12 `IN_FORMATS` blob：
   支持的 modifier 只有 `LINEAR(0)` 与几个 vendor=ARM(0x08) 的
   `*_FBDC_TILE`/AFBC 值（11/12/13/33/34）——**22 和 31 根本不在这块 plane
   的支持列表里**，内核拒绝是完全正确的行为，不是驱动 bug；
3. 查 BSP 源码（sysroot `usr/src/debug/`）实锤两点：
   - `libomxvpu/1.0/src/omx_vpudec_component.c:990`：非 CODA（即 WAVE）核心
     `pVpu->decOP.wtlEnable` **恒为 TRUE**——WAVE 硬件自带 Write-To-Linear，
     解码输出默认就是硬件去平铺后的**真 LINEAR** 帧，不是软件 CPU 转换；
   - `gstreamer1.0-omx/.../omx/gstomxvideodec.c:1728-1730`：只有协商 caps
     显式带 `GST_CAPS_FEATURE_MEMORY_IFBC` 时才会把 `eColorFormat` 切成
     `OMX_SEMI_COLOR_FormatIFBC32x8Tiled`（真正的 tiled/压缩输出）；本管线
     appsink 协商的是纯 `video/x-raw(memory:DMABuf)`，不带该 feature，因此
     **拿到的必然是 WTL 之后的线性 NV12**；
   - 同一份 `gstomxvideodec.c` 里 `gst_buffer_add_video_meta` **从未被调用**
     ——之前代码注释"GstVideoMeta omx dmabuf 输出一般带"是错的，这条链路上
     `vm` 恒为 `nullptr`，pitch 实际总是走 caps 推算的紧凑值；
4. 修复后在设备上实跑一次（`start.sh` 前台 10s），新增的诊断日志
   `buf_bytes=2764800` 与 `tight_nv12_bytes=2764800`（1920×960×1.5）**完全
   相等**——实测证实该分辨率下 dmabuf buffer 无行对齐 padding，caps 推算的
   pitch 是对的；同时 `[drm]` 日志里 `addFB2WithModifiers rejected` 的刷屏
   **完全消失**（`drm_modifier=0` 后直接走隐式 LINEAR 分支，不再尝试错误
   modifier）。

**结论与修复**：`drm_modifier` 默认值由 `0x0800000000000016`（错误猜测的
WAVE_32X8_TILE）改回 **`0`（LINEAR）**——这才是与硬件实际输出布局及 plane
真实支持能力都吻合的值。改动点：`config/androidauto.ini`、
`src/model/AppConfig.h`（`PlayerOptions::drmModifier` 默认值）、
`src/media/MediaPlayer.cpp`（注释更正 + 新增 first-frame 诊断日志）。

**验证状态**：✅ 日志层面（无拒绝刷屏 + buffer 大小与紧凑布局精确匹配）；
⏳ **像素级视觉确认待补**——下次人工跑 launcher→播放全流程时肉眼确认视频画面
是否仍有"不纯净"观感。若肉眼确认已解决，此项可从待办移除；若仍有残留问题，
说明"画面不纯净"另有成因（候选：色彩空间/YUV 矩阵、alpha/blend 残留、
双 fb 轮换与 VPU 输出 buffer 池生命周期竞争——见 §9.2 `MediaPlayer` 双模式条目
与行动书 R3 风险），需继续排查，不要退回猜测式 modifier 注入。

### 9.5 追加排查：点击 Auto 应用不出画面（用户误判为 plane-id 变更）

**背景**：§9.4 修复部署后用户反馈"重启之后 plane-id 可能变了，点击 auto 应用
没有播放视频"，并提议在 util 目录加一个"获取可用 plane-id"的工具函数。**先用
日志核实了这个猜测，证据不支持**：两次实测（出问题的那次 + 之后紧接的对照
组）里 `[drm] plane 43 free overlay (type=0)` / `ready: plane=43 ... blend=none`
都成功——plane 选择逻辑（`DrmView::pickPlane`，`drm_plane_id=0` 自动挑选）全程
正常，没有失败或换 plane 的迹象。**真正的故障点在 VPU 解码器本身**：

```
[media] error: ...gstomxvideodec.c(3704): gst_omx_video_dec_handle_frame ():
  OpenMAX component in error state Stream corrupt (0x8000100b)
```

查 BSP 源码 `libomxvpu/1.0/src/omx_vpudec_component.c:2071-2077`：该错误码
（`OMX_ErrorStreamCorrupt`）在 `VPU_DecCompleteSeqInit`（解析 SPS/VPS/PPS
序列头）失败时抛出——即 VPU 在这次会话的最开头就没能完成初始化，之后再没
产出任何一帧（appsink 从未收到样本，视频区自然一直空白）。**这本身大概率是
单次瞬时性 VPU/驱动小概率失败**（原地不改任何代码重跑一次，序列头解析就
成功了，见下方"验证"）。

**但暴露了一个更严重的次生问题**：`MediaPlayer::busWatch()` 收到
`GST_MESSAGE_ERROR` 时，只对"音频 bin 报错"这一种情况做了处理（拆掉音频会话
保视频），对**视频 bin 报错什么都不做**——错误态的 `dec`/`appsrc` 元素继续挂
在管线上，appsink 永远收不到新样本，但 `FilePacketFeeder` 对此完全不知情：
按文件时长每 ~16s 照常 EOS 重建自己那半条链路、继续往 `v_queue` 里推包。
实测现象：`v_queue` 卡在故障时刻的水位（如 `100000000us/6300B`）**几分钟纹丝
不动**，`[feeder] loop: restart` 每 16s 正常打印、`[diag]` 每 2s 正常打印，
表面看"进程活着、日志在滚"，但视频区永久空白，且没有任何明确的"视频已失败"
信号——这才是用户"点了没反应"的真正体验：即便 VPU 只是偶发失败一次，应用也
永远无法自愈，必须杀掉进程重开才能恢复。

**修复**：`busWatch()` 里对称加了视频 bin 分支——错误源是 `vBin_` 的后代时，
调用 `stopVideoSessionLocked()` 拆掉视频会话（连带清空 `vCapsStr_`/
`vCapsPending_`）。下一次 `FilePacketFeeder` 循环（本来就会发生，不需要新增
定时器）调用 `setVideoCaps()` 时会发现 `vSrc_==nullptr`，`setVideoCaps()` 里
已有的逻辑会自然触发 `startVideoSessionLocked()` 重建整条视频链，相当于给
VPU 一次重新初始化的机会。**没有引入 `videoDead_` 式的"永久放弃"标志**（对比
§9.1 #2 的 `audioDead_`）：audio 那条路径需要死标志是因为它当时的 bug 会导致
"每包都重试"的死循环刷日志；视频这条路径的重试节奏天然被 feeder 的循环周期
（~16s，取决于素材时长）限速，跟这个文件源本来就 `loop=1` 无限循环播放的设计
是一致的，不需要额外复杂度。改动点：`src/media/MediaPlayer.cpp`
`busWatch()`。

**验证**：原地不改代码重跑一次（`start.sh` 前台 38s，覆盖两轮 feeder 循环）
——**完全正常**：首帧解码成功、`v_queue` 全程 `0us/0B`（解码器实时消费，
不再堆积）、音频也顺带正常起来了、两次 EOS 循环重建都无缝衔接、`[drift]`
纠偏持续保持同步。故障没有复现，符合"瞬时性初始化失败"的判断；即便下次再遇到
同类 VPU 瞬断，新加的恢复路径也会在下一个 feeder 周期内自动重建视频会话，
而不是永久卡死。

### 9.6 真正的"画面不纯净"根因：chroma plane offset 从未被赋值（绿幕）

**背景**：§9.4/§9.5 修完后用户反馈画面依然不纯净，具体表现是**一层绿色**，
怀疑是 BGR 颜色反转（这条链路其实全程是 YUV，从没转换成 RGB，所以不是字面
意义的"BGR 反转"，但用户对"色彩通道错位"的直觉方向是对的）。

**根因**：[MediaPlayer.cpp](src/media/MediaPlayer.cpp) `pullVideoFrame()` 里
按 plane 填充 dmabuf 描述符的循环中，`vm == nullptr`（§9.4 已证实这条链路上
恒为 nullptr）分支只赋值了 `frame_.pitch[p]`，**从未赋值 `frame_.offset[p]`**。
`frame_` 在函数开头用 `VideoFrame{}` 默认初始化，`offset[4]` 的默认值是
`{0,0,0,0}`，所以 chroma plane（NV12 的 UV 交织平面，p=1）的 offset 一直是
**0**——等于告诉 `DrmView`/内核"UV 数据从 buffer 起始字节开始"，而 buffer
起始处实际是 Y（亮度）平面的数据。显示硬件把亮度字节当色度（U/V）采样：
YCbCr→RGB 转换公式里 R、B 分量依赖 `(Cr-128)`、`(Cb-128)`，被喂进去的其实是
远离中性值 128 的亮度字节，R/B 大量被推向负值截断到 0，只剩 G 分量占主导——
这正是"整屏偏绿"的经典成因，纯 plane 布局 bug，跟 RGB/BGR 顺序、色彩空间
矩阵、alpha blend 都无关。

**修复**：用 `GST_VIDEO_INFO_PLANE_OFFSET(&info, p)` 补上 else 分支里缺失的
offset 赋值——与已经在用的 `GST_VIDEO_INFO_PLANE_STRIDE(&info, p)` 同一个
`GstVideoInfo`、同一套"紧凑无 padding"假设，逻辑一致不引入新假设。顺手把
`pitch0`/`offset1` 也加进首帧诊断日志方便下次核对。

**验证**：设备实测首帧日志 `pitch0=1920, offset1=1843200`——`1920×960=
1843200`，与 luma 平面大小精确相等，是 NV12 紧凑布局下 chroma plane 应有的
起始偏移。（视觉上"绿幕是否消失"仍需用户下次播放时肉眼确认，但这个 offset
错误本身已经是确凿无疑的 bug，且是三次排查里第一个能直接解释"整屏偏绿"这个
具体现象的根因——§9.4 的 modifier 问题和 §9.5 的解码器崩溃都不会产生这种
颜色特征。）

**教训**：这条 offset 逻辑从 v1.3 DRM 直扫路线一开始写下就带着这个漏洞，
`vm != nullptr` 分支正确、`else` 分支只补了一半——因为实际运行路径上 `vm`
从未非空过，这半个分支实际上从未被正确验证过。以后新增"理论上会用到，但
当前从没实际触发过"的分支时，要么补充断言/日志确认分支确实被执行到，要么
干脆先证实这个分支会走到再写完整。

**给用户的结论**：不需要新增"获取可用 plane-id"的工具函数——plane 选择这条
链路这次全程没有出过问题，日志证据很清楚。真正需要的是"视频链出错后自愈"，
已经按上面的方式修好。

### 9.7 loop=0 测试暴露的两个问题：EOS 不退出、退出流程死锁

**背景**：用户把 `loop=0` 想验证"播放完是否回到 launcher"，实测是播放完停在
末帧、动鼠标也不重绘回 launcher。排查发现是两个独立问题叠加。

**问题一：非循环 EOS 从未触发退出**。`FilePacketFeeder` 的 `hooks_.onEos`
在 [AndroidAutoApp.cpp](src/app/AndroidAutoApp.cpp) 里原来只调用
`media_->endVideoStream()`，注释写的是"应用继续显示末帧"——这是当初的
**故意设计**，不是漏写，但不符合用户想要的"播放完回到 launcher"效果。DRM
直扫模式下视频 plane（zpos=2、blend=none 纯覆盖）不会自己消失，会一直挡住
下层 weston 画面，鼠标移动触发的 weston 重绘也无济于事（重绘的是被挡住的
下层）。**修复**：`onEos` 里加一行 `model_.quit()`——跟 ✕ 按钮/SIGTERM 走
同一条已验证过的退出路径（`onQuitCheckThunk` 每 200ms 轮询
`model_.running()`，这条路径本来就是给"非 wayland 事件来源的退出请求"设计
的，EOS 正好是这一类）。

**问题二（更严重）：退出流程本身会死锁**。加完问题一的修复后实测：EOS 后
`model_.quit()` 确实被调用，但 `AndroidAutoApp::teardown()` 卡死在
`media_->stop()` 里的 `gst_element_set_state(pipeline_, GST_STATE_NULL)`——
`adb shell` 查 `/proc/<pid>/task/*/stack` 看到几乎所有 streaming 线程
（包括 VPU 解码器自己的输出线程 `v_dec:src`）全部阻塞在 `futex_wait`，
SIGTERM 送达也不起作用（"signal received, quitting"能打出来，但卡在后面），
只能 SIGKILL。同时 `dmesg` 反复出现内核警告：

```
WARNING: CPU: 1 PID: 170 at /drivers/dma-buf/dma-buf.c:116 dma_buf_release+0x98/0xa0
Call trace:
 dma_buf_release+0x98/0xa0
 __dentry_kill / dput / __fput / delayed_fput / process_one_work / worker_thread
```

**根因推断**：`DrmView` 的 `current_`/`prev_` 通过 `drmPrimeFDToHandle` 对
VPU 输出的 dmabuf 建立了一份**独立于 GStreamer 引用计数之外**的 GEM 句柄
引用。原来 `teardown()` 的顺序是先 `media_->stop()`、后面才轮到
`drmView_.clear()`/`destroy()`（释放这些 GEM 句柄）。vendor OMX/VPU 驱动
关闭解码器时，很可能要等自己导出的 dmabuf 被**所有 importer 都 detach**才
能完成状态切换——但这时 DrmView 那份 GEM 引用还没归还，OMX 驱动在等一个
永远不会来的释放信号，形成跨子系统（GStreamer 引用计数 vs DRM/dma-buf 引用
计数）的死锁。dmesg 里的内核警告与此吻合（虽然 WARNING 本身不是致命错误，
不会导致内核 panic，但指向同一处 dma-buf 生命周期管理）。

**修复**：调整 `teardown()` 顺序——DRM 分支收尾（`evdev_.stop()` +
`drmView_.clear()` + `drmView_.destroy()` + repaint 触发 weston 重绘）挪到
`media_->stop()` **之前**。这样等 OMX 解码器要关闭时，外部（DRM 侧）已经不
再持有它导出的 buffer，状态切换才能正常走完。顺手在视频 appsink 上加了
`wait-on-eos=FALSE`（appsink 默认收到 EOS 时会在 streaming 线程里等应用把
已入队样本全部 pull 完，见 `gstappsink.c gst_app_sink_event()` 的
`GST_EVENT_EOS` 分支——这次死锁的主因不是这个，但这是另一个真实存在、
本应用用不上"EOS 前必须排空"这条保证的阻塞点，顺手关掉更稳）。

**验证**：设备实测（`loop=0`，完整跑满 963 帧到自然 EOS）——`[feeder] end of
stream` → `[app] exit` → `[feeder] stopped` → `[drm] plane cleared, weston
repaint triggered` 依次打出，进程随后**完全退出**（`ps`/`/proc` 反复确认
不再存在，无需 SIGKILL）；`modetest -M semidrive -p` 确认 plane 43 变回
`FB=0 CRTC=0`（干净释放，不再遮挡 weston 画面）。dmesg 的 dma_buf_release
警告仍会出现（内核侧已知的非致命小毛病，不阻塞退出，暂不处理，记录在此
供以后排查参考）。

**改动点**：`src/app/AndroidAutoApp.cpp`（`vh.onEos` 加 `model_.quit()`；
`teardown()` 调整 DRM 收尾与 `media_->stop()` 的顺序）、
`src/media/MediaPlayer.cpp`（视频 appsink 加 `wait-on-eos=FALSE`，drm 路径
与 shm 路径两处都加了）。

### 9.8 §9.7 的顺序修复不够：vendor OMX 关闭本身不可靠，加看门狗强杀

**背景**：§9.7 的顺序修复部署后，命令行测试（`adb shell` 直接跑
`start.sh`）多次验证都是干净退出。但用户从 launcher **真实点击图标**测的
第一次就复现了旧问题：视频播完，launcher 左上角还留着 AndroidAuto 的窗口
（标题栏+边框+✕，这是应用自己的 wl_surface，跟已经清掉的 DRM 视频 plane是
两回事），点 ✕ 没反应，再从格子里点图标也不放视频了。

**排查**：`ps -ef` 发现真机 launcher 启动的那个进程（weston 用户）还活着，
`/proc/<pid>/task/*/stack` 一查，跟 §9.7 死锁前一模一样的签名——`v_dec:src`
等 streaming 线程全部卡在 `futex_wait`，主线程卡在 `media_->stop()` 里。
`md5sum /proc/<pid>/exe` 确认它跑的就是带 §9.7 修复的最新二进制。**结论**：
§9.7 的顺序调整解决的是一类真实存在、可复现的死锁根因（DrmView 的 GEM
引用没提前释放），但没有、也不可能完全消除 vendor OMX/VPU 关闭流程本身的
不稳定——同一份代码，命令行跑几次干净退出，真实点击那次又卡死，说明这是
闭源驱动的偶发行为，不是我们代码能保证百分百确定性修复的 bug。

**由此重新评估用户提的两个方案**：
1. "改成播完能点 ✕ 退出"——不成立。✕ 按钮本来就和 EOS 走的是同一条
   `model_.quit()` 路径，问题从来不是"没接好"，是接好之后卡在
   `media_->stop()` 里，这时主循环已经不再处理任何输入事件，✕ 点了也没用；
2. "不好修就直接退出不显示窗口"——治标不治本。不显示窗口不会让卡死的进程
   消失，`ivi_surface id=3010` 还是被占着，下次点图标该进不去还是进不去。

**真正的修复**：给 `media_->stop()` 包一层限时看门狗——开一个 detached
`std::thread` 睡 2 秒，如果这期间主线程还没从 `media_->stop()` 返回（即
`teardown()` 没能正常走完），看门狗直接 `_exit(1)` 强制整个进程退出。放在
`media_->stop()` 之前的 DRM 收尾（§9.7 已经确保先做）不受影响，plane 和
weston 重绘该多快还是多快；看门狗只保证"就算 OMX 关闭卡死，进程本身也一定
会在有限时间内消失"，从而让 wl_surface 断连（launcher 里的窗口跟着消失）、
`ivi_surface id` 得到释放（下次点图标能正常绑定）。2 秒是留了余量的经验值：
正常干净退出全程通常 <1 秒，卡死时哪怕等满 2 秒对用户来说也比"永久卡住只能
物理重启"体验好得多。

**验证**：连续两次设备实测（真实 `start.sh` 拉起，非 adb 命令行绕过launcher
的方式）：
- 第一次：`stderr` 捕获到 `[app] teardown watchdog: media stop exceeded
  timeout, forcing exit`——确认 `media_->stop()` 这次又卡住了（复现了闭源
  驱动的不稳定），2 秒后看门狗按预期强制退出，`ps`/`modetest` 确认进程
  彻底消失、plane 43 回到 `FB=0 CRTC=0`；
- 紧接着立刻重新拉起第二次：`[app] ivi surface id=3010` 绑定成功、完整播完
  963 帧、正常 EOS 退出——证明第一次的强杀确实释放了 `ivi_surface id`，
  连续重新启动不再受阻。

**残留认知**：`media_->stop()`/vendor OMX 关闭的根本不稳定性没有被消除，
只是被兜底了；具体触发条件（是否和是否有音频会话、之前是否发生过强杀等
历史状态有关）还不清楚，暂不深挖（闭源组件，性价比低）。dmesg 里
`dma_buf_release` 的内核 WARNING（见 §9.7）大概率与此同源，也一并观察即可，
不单独处理。

**改动点**：`src/app/AndroidAutoApp.cpp`（`teardown()` 里 `media_->stop()`
调用前加 2 秒看门狗线程，超时 `_exit(1)`）。

### 9.9 CursorOverlay：独立 plane 画鼠标光标（调试用，可整体删除）

**背景**：evdev 坐标上报（§9.2）能正确捕捉点击（`[evdev] video-area click:
screen=(940,627) video=(940,926)...`），但鼠标光标在视频画面上不可见，没法
凭肉眼确认这坐标对不对。查了这块 DPU 枚举出的所有 plane（`modetest -M
semidrive -p`），**没有一个 type 是 Cursor**——只有 Overlay 和 Primary，
说明这颗芯片没有独立硬件光标 plane，weston 的指针是软件合成进它自己 GL
渲染的 primary 画面里的。这是结构性问题，不是 zpos 数值没调对：只要视频
plane（zpos=2、blend=none 全覆盖）盖在 primary 之上，指针必然被挡住。

**方案**：新增 `src/util/CursorOverlay.h/.cpp`——完全独立于 `DrmView` 的
一个小工具类，自己开一路 `/dev/dri` fd，用 DRM dumb buffer（不走
dmabuf/GStreamer，纯 `DRM_IOCTL_MODE_CREATE_DUMB` + mmap + cairo 画一个
20×28 的箭头位图）在视频 plane 之外再占一个空闲、支持 ARGB8888 的 overlay
plane，zpos 设到 3（比视频的 2 更高——当年设计 zpos 时注释里"顶层 RGB 层"
预留的正是这个位置）。`moveTo()` 只挪 plane 目标矩形，不重绘内容，开销
是一次 `drmModeSetPlane` ioctl。失败（没有多余的 ARGB overlay plane）只
记日志、返回 false，不影响视频路径。

**改动点**（全部是增量，没有改动任何既有函数的行为）：
- 新增 `src/util/CursorOverlay.h/.cpp`；
- `src/view/DrmView.h` 加一行只读 getter `crtcId()`（供 CursorOverlay 复用
  同一 crtc，不改 DrmView 任何逻辑）；
- `src/input/EvdevReader.h/.cpp` 加一个可选的 `MoveFn` 回调（每次
  REL_X/REL_Y 更新坐标后触发，跟原有仅按下沿的 `PointerFn` 完全独立、
  互不影响；`start()` 新增参数带默认值 `nullptr`，不传就是原来的行为）；
- `src/app/AndroidAutoApp.h/.cpp`：加 `cursor_` 成员，在 `drmView_.create()`
  成功、`evdev_.start()` 之前调用 `cursor_.create(...)`；`evdev_.start()`
  多传一个 lambda 把移动坐标转给 `cursor_.moveTo(...)`；`teardown()` 的
  DRM 收尾块里 `evdev_.stop()` 之后加一行 `cursor_.destroy()`（在
  `media_->stop()` 之前，跟 §9.7 的顺序要求一致，不受 OMX 关闭是否卡死
  影响）。

**要整体移除时**（比如触摸驱动落地后，触摸没有光标概念）：删掉
`CursorOverlay.h/.cpp` 两个文件 + 上面 `AndroidAutoApp.h/.cpp` 里标了
`cursor_`/`CursorOverlay` 的几行，`DrmView::crtcId()` 和 `EvdevReader::
MoveFn` 留着不影响任何东西（纯加法，没人调用等于没有），不强制一起删。

**验证**：设备实测，`[cursor] ready: plane=54 zpos=3 size=20x28`——找到了
一块视频 plane（43）之外的空闲 ARGB8888 overlay plane，创建成功，播放
全程正常；这次测试恰好又复现了一次 §9.8 的 vendor OMX 关闭卡死，看门狗
2 秒后强制退出，`modetest` 确认 plane 54 也随 `cursor_.destroy()`（在
`media_->stop()` 之前执行）干净释放（`FB=0 CRTC=0`），没有被 OMX 那边的
卡死连累——证明这个新增功能没有引入新的退出风险。**光标是否真的跟手可见
仍需用户下次操作时肉眼确认**（日志/plane 状态只能证明"这块地方有一个箭头
在正确的坐标"，最终效果还是要看屏幕）。

### 9.10 CursorOverlay 移动卡顿：legacy setplane 与视频共用 CRTC 抢锁

**现象**（用户反馈）：移动鼠标光标时视频会卡。

**根因**：`CursorOverlay::moveTo()`原来对每个 evdev `REL_X/REL_Y`事件都
同步调用一次 legacy `drmModeSetPlane`（跟 `DrmView`提交视频帧用的是同一套
接口），而且传的是同一个 `crtcId_`——内核对同一 CRTC 的 plane 提交是串行
化的，很多驱动的 legacy SetPlane 还会等到下一个 vblank 才返回。evdev 原始
移动事件率可到几百 Hz，远超屏幕刷新率，逐事件同步提交会在主线程（evdev
的 onMove 回调与视频帧拉取/提交同在 glib 主循环）里跟视频那次提交抢锁，
表现为"移动光标时视频卡顿"。

**修复**：`moveTo()`按 ~60Hz（`g_get_monotonic_time()`卡 16ms 最小间隔）
节流合并——窗口内的移动只更新 `pendingX_/pendingY_`，不发 ioctl；真正提交
时带最新坐标。若节流窗口关闭时还有未提交的最新位置，用一次性
`g_timeout_add`定时器补发，保证最终坐标不丢、不会卡在中途。`destroy()`
里显式 `g_source_remove`清理这个定时器，避免残留回调访问已关闭的 fd。
改动全部在 `CursorOverlay.h/.cpp`内部。

**验证**：交叉编译零警告零错误，已部署。跟手是否流畅、卡顿是否消失仍需
用户下次从 launcher 启动后实测确认——自动化 adb 压力测试因 adb shell 后台
任务本身的进程组回收时机问题（跟这次代码改动无关）没能跑通，改走真实
launcher 路径验证。

### 9.11 音频权限修复：weston 用户加入 audio 组（设备侧，rootfs 只读绕过）

**背景**：§7 待办 3 早就记录了这个已知问题；本次会话用户反馈"单独用指令
测试 ALSA 正常，但 app 里报 `[media:audio] sink open failed (ALSA
unavailable)`"，借此把它实际修掉了。

**根因**：`/dev/snd/*`（`pcmC1D0p`等）权限是 `root:audio 0660`；AndroidAuto
由 weston 的 ivi-launcher 拉起，运行身份是 `weston`用户，其补充组
（`weston,input,video,wayland,render`）里**没有 `audio`**，`snd_pcm_open`
必然 Permission denied，触发 [MediaPlayer.cpp:368-384](src/media/MediaPlayer.cpp#L368-L384)
的 ALSA 预探测保护逻辑，跳过音频保视频（这段保护逻辑本身没问题，是环境
缺陷）。用户手动测试走的是 `adb shell`（root 身份），root 不受这个 0660
限制，所以单独测试是通的——这就是两边表现不一致的原因。

**修复**（设备侧，`/` 只读，按 §4 怪癖清单第 7 条的既定流程操作）：
```
mount -o remount,rw /
sed -i 's/^audio:x:29:$/audio:x:29:weston/' /etc/group   # 备份先存到 /data（/ 是 ro，/tmp 是 tmpfs 重启即丢）
mount -o remount,ro /
systemctl restart weston                                 # 补充组是进程启动时从 /etc/group 解析的，必须重启才生效
```
备份文件：`/data/group.bak_1789737813`（原始 `audio:x:29:`一行，误操作可
用它手动恢复）。

**验证**：`systemctl restart weston`后 `is-active`=active；新 weston 进程
`cat /proc/<pid>/status | grep Groups`确认已包含 gid 29（audio）。**这只是
运行时修复，重刷机会丢**——要长期生效需要把这条用户组关系落进板子 BSP 的
用户/组配置（比如 usergroup 的 recipe 或 systemd-sysusers 规则），不是本
应用能做的事,标记为后续 BSP 侧硬化项。**实际声音是否正常仍需用户下次播放
时用耳朵确认**（`[media:audio]`那行日志不再打印"sink open failed"只能证明
设备打开成功，不能证明声音本身对不对——采样率/声道/爆音等问题日志层面
看不出来）。
