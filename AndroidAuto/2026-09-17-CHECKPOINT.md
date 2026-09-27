# AndroidAuto 项目 Context Checkpoint

> **用途**：新开 AI 会话时，让助手快速恢复进度上下文，从此处继续辅助开发。
> **更新约定**：每完成一个阶段/修复一个重要问题，更新本文档的「当前状态」与「待办」。
>
> | | |
> |---|---|
> | 更新时间 | 2026-09-17 20:30 |
> | 项目阶段 | v1.2 统一媒体管线（screencast-avsync-design 落地）完成，音视频播放+音画同步+循环全通，端到端验证通过 |
> | git 最新 | `ebbb3de`（v1.0 提交；v1.1/v1.2 改动**待提交**） |

---

## 0. 新会话冷启动指南（AI 先读这个）

1. **读文档顺序**：本文件 → `README.md`（构建/部署/配置/板级约束）→ `2026-09-17-media-pipeline-appsrc-live-deadlock.md`（最新踩坑复盘，含 appsrc live 死锁根因）→ 需要时 `2026-09-16-CHECKPOINT.md`（v1.0 历史）/ `TEST_PLAN.md`
2. **构建**：`. /home/admin0412/x9sp_wayland/environment-setup-cortexa55-sdrv-linux && make`（项目根目录）
3. **部署**：`./push.sh`（推运行时文件+重启 weston）或 `./push.sh --no-restart`
4. **设备**：adb（root），x9m_ms，Weston 13 定制 ivi-shell；**媒体运行需 x9hp 环境**（AAC 软解在 `/opt/x9hp/lib/gstreamer-1.0/libgstlibav.so`），入口是 `/data/AndroidAuto/start.sh`（source /opt/x9hp/env.sh 后 exec 二进制）
5. 严禁凭记忆写 wayland 回调签名——**先查 sysroot 头文件**；严禁改动第三方库/设备系统文件
6. 媒体架构问题先读 `src/media/MediaPlayer.h` 头注释（设计文档 § 对应关系都在里面）

## 1. 项目是什么

- **AndroidAuto**：ivi-shell wayland 客户端应用（C++17 / MVC / cairo / RAII / glib 主循环 / GStreamer）
- **v1.2 媒体架构**（screencast-avsync-design.md 落地）：
  - **单管线 + 固定 GstSystemClock**：`MediaPlayer`（src/media/）持有 GstPipeline，音视频两个 bin（GstBin）动态挂拆，时钟不随分支增删切换
  - **统一媒体包接口**：`pushVideoPacket/pushAudioPacket`（任意线程，block 反压不丢包）——文件与投屏同一路径
  - **FilePacketFeeder**（src/media/）：filesrc!parsebin 拆包，fakesink **sync=TRUE 1x 重放**（给文件源造出实时到达节奏），EOS 循环 = **整管线重建 + ptsOffset 累加**（parsebin FLUSH seek 不可靠）
  - **显式解码链**：视频 `queue→h265parse→omxh265dec(硬解)→videoconvert(BGRx)→appsink(sync=FALSE)`；音频 `queue→aacparse→avdec_aac(软解)→convert→S16LE/2ch/48k→alsasink(sync=TRUE)`；解码器按首包 caps 选定（h264/h265，硬解缺落 libav 软解）
  - **音画纠偏**：仅两路并存时运行，2s 周期，±20ms 死区，小步长（≤5ms）只调视频 appsink ts-offset
  - **卡顿检测+IDR 恢复**（投屏机制）：`stall_detect` 开关，**文件源必须关**（误触发会毁时间线）
- 视频帧 BGRx 零拷贝（=cairo RGB24 内存序）绘入应用窗口播放区（`video_margin` 可调，默认 20px）
- 时延四点链路：读取点（push 时刻）→拉出→提交→上屏（wl_surface_frame done），`[latency]`/`[drift]`/`[diag]` 日志 + 退出总结
- 布局：tiling 分格自适应（480×325）；full_screen 切换代码就绪但被 hmi-controller bind 权限阻塞（见 §4.12）
- 目录：`/home/admin0412/windows_share/x9_linux/data/AndroidAuto/`

## 2. 关键环境事实（host）

| 项 | 值 |
|---|---|
| SDK | `/home/admin0412/x9sp_wayland/`（wayland 1.22 + cairo 1.18 + **glib/gio + gstreamer 1.22 开发包齐全**） |
| 备用 SDK | `/home/admin0412/x9sp/`（缺 wayland 开发包，勿用） |
| 参考实现 | `/home/admin0412/projects/ivi_app/`（ivi_video_player 等，TEST.md 踩坑实录权威） |
| 设计文档 | 项目内 `screencast-avsync-design.md`（媒体架构蓝图）+ `video_audio_pipeline_design.md`（file/packet 统一入口） |
| 源码编辑 | **勿用 Windows 编辑器保存**——会把文件转 CRLF 导致 patch 失配（已发生过，用 dos2unix/sed 修复） |

## 3. 设备侧状态（x9m_ms）

| 项 | 状态 |
|---|---|
| weston 13 + ivi-shell | ✅ 运行中（start_ivi.sh bind-mount hmi-controller 等） |
| 播放入口 | `/data/AndroidAuto/start.sh`（launcher 图标也指这里；**必须经它启动**，直接跑二进制无 AAC 软解） |
| 视频 | ✅ omxh265dec VPU 硬解（`/dev/vpucoda /dev/vpuwave`，start_ivi.sh 修权限） |
| 音频 | ✅ avdec_aac 软解（/opt/x9hp libgstlibav）→ ALSA card1 TLV320AIC23 S16_LE/2ch/48000 |
| 验证数据 | 首帧 ~400ms；pull→commit ~7ms；commit→screen ~10ms；drift 收敛 +7.5ms in sync；循环多轮正常；D-1 启停 5/5 |

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
11. **appsrc is-live=TRUE 与动态挂载 bin 的 preroll 互锁**（静默死锁，本次核心坑，详见复盘文档）
12. hmi-controller bind 权限校验：仅 weston-ivi-shell-user-interface 可绑 ivi_hmi_controller，其他客户端 bind 即 fatal 协议错误；full_screen 路线等厂商放行（应用侧 switchLayout 已就绪，ini `fullscreen_layout=1` 即启用）
13. parsebin/qtdemux 对 FLUSH seek 不可靠——循环用整管线重建
14. gst-launch 对照组里 demux pad 与 sink 之间必须加 queue，否则对照组自己假卡死

## 5. 关键文件地图

```
src/
├── app/AndroidAutoApp.*    组合根：glib 主循环/wayland fd/信号/时延统计/装配
├── app/InputController.*   输入→关闭等语义
├── media/MediaTypes.h      Transport profile/VideoFrame
├── media/MediaPlayer.*     ★统一管线：单管线+固定时钟+动态 bin+包接口+纠偏+卡顿检测
├── media/FilePacketFeeder.* ★文件→包桥：parsebin 拆包+1x 重放+循环重建
├── model/AppConfig.*       ini 解析（[app]/[window]/[player]/[ui]）
├── model/AppModel.h        窗口/视频尺寸/dirty
├── view/AppView.*          cairo 绘制（BGRx 零拷贝绘帧）
├── view/Layout.*           几何（videoWindow 宽高比自适应）
├── wayland/WlClient.*      连接/ivi_surface/输入/hmi-controller(按需绑)
├── wayland/ShmBuffer.h     wl_shm 双缓冲
└── util/Log.h              RAII 日志
config/androidauto.ini      全参数（player 段见 §6）
start.sh                    设备启动包装（x9hp 环境）
push.sh                     部署（含 start.sh）
protocol/                   ivi-application + ivi-hmi-controller XML
```

## 6. [player] 配置速查（config/androidauto.ini）

| 键 | 当前值 | 说明 |
|---|---|---|
| video_uri | h265_aac_1920x960_60fps.mp4 | 文件源（feeder 拆包出两路） |
| audio_uri | 空 | 空 = 同 video_uri；独立源须 AAC |
| loop | 1 | EOS 循环（管线重建+PTS 偏移） |
| transport | usb | usb=queue 100ms/卡顿 60ms；wifi=500ms/300ms |
| stall_detect | 0 | **文件源必须 0**（投屏网络机制） |
| drift_interval_sec | 2 | 音画纠偏周期 |
| latency_trace / latency_interval | 1 / 60 | 时延统计 |
| fullscreen_layout | 0 | 需 compositor 侧放行（§4.12） |

## 7. 待办事项（按优先级）

1. **投屏协议层接入**：MediaPlayer 包接口已就绪（push*Packet/setCaps/start*Session），协议层拿到 USB/WiFi 裸流直接调用；接入时需验证 is-live=FALSE 在真实网络包下的表现（复盘中标"待验证"）+ 开 stall_detect + IDR 请求通道对接
2. **v1.1+v1.2 git 提交**（用户确认后；建议拆两笔：v1.1 双 playbin、v1.2 统一管线）
3. D-2 长时间内存复测（v1.2 架构下跑一次 5 分钟采样确认封顶行为不变）
4. full_screen 布局放行（BSP/厂商：hmi-controller bind 校验放行，材料在 README §2.2）
5. 鼠标实测 launcher 点击启动 → 播放 → ✕ 关闭（用户手动）
6. 触摸驱动（BSP 范围，CONFIG_TOUCHSCREEN_SEMIDRIVE=y）

## 8. git 历史

```
ebbb3de 排查沉淀：ivi 集成期五故障复盘      ← v1.0 末次提交
（工作区：v1.1 双 playbin + AAC 接入；v1.2 统一媒体管线重构 + appsrc 死锁修复，
 待提交；TEST_PLAN/README/两份排查文档已同步更新）
```

## 9. 本次会话（v1.2）修复的问题速览

| # | 问题 | 修复 |
|---|---|---|
| 1 | feeder sync=FALSE 整文件瞬间灌入 | fakesink sync=TRUE 1x 重放 |
| 2 | decodebin+appsrc 协商挂起 | 弃 decodebin，显式链路（按 caps 选 parser/解码器） |
| 3 | **appsrc is-live=TRUE 与动态 bin preroll 死锁（根因）** | is-live=FALSE，节奏交给 feeder |
| 4 | base_ts==0 哨兵与文件 PTS=0 冲突 | 显式 baseTsSet_ 标志 |
| 5 | stall 误触发毁时间线 | stall_detect 开关 + 2s 确认窗口 + 首帧后才武装 |
| 6 | parsebin FLUSH seek 二轮无数据 | EOS 整管线重建 + ptsOffset 累加 |
| 7 | 时延配账串帧（负数） | frameCbPending_ 期间不更新 pendingLat_ |

详细证据链见 `2026-09-17-media-pipeline-appsrc-live-deadlock.md`。
