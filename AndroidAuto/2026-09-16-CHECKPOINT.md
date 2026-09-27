# AndroidAuto 项目 Context Checkpoint

> **用途**：新开 AI 会话时，让助手快速恢复进度上下文，从此处继续辅助开发。
> **更新约定**：每完成一个阶段/修复一个重要问题，更新本文档的「当前状态」与「待办」。
>
> | | |
> |---|---|
> | 更新时间 | 2026-09-16 21:00 |
> | 项目阶段 | v1.1 音视频播放完成，端到端验证通过（tiling 布局下） |
> | git 最新 | `ebbb3de`（6 个提交，仓库即本目录；v1.1 改动待提交） |

---

## 0. 新会话冷启动指南（AI 先读这个）

1. **读文档顺序**：本文件 → `README.md`（构建/部署/配置）→ `2026-09-16-androidauto-ivi-integration-bugs.md`（踩坑复盘，含设备怪癖）→ 需要时 `DEV_PLAN.md` / `TEST_PLAN.md`
2. **构建环境**：`. /home/admin0412/x9sp_wayland/environment-setup-cortexa55-sdrv-linux && make`（在项目根目录）
3. **部署**：`./push.sh`（推运行时文件+重启 weston）或 `./push.sh --no-restart`
4. **设备访问**：`adb`（root），设备为 SemiDrive x9m_ms，跑 Weston 13 定制 ivi-shell
5. 严禁凭记忆写 wayland 回调签名——**先查 sysroot 头文件**（见 §4 怪癖清单）

## 1. 项目是什么

- **AndroidAuto**：ivi-shell wayland 客户端应用（C++17 / MVC / cairo / RAII / glib 主循环 / GStreamer）
- 功能：launcher 图标点击启动 → 应用窗口（标题栏+logo+右上角 ✕ 关闭+播放区）→ **音视频播放**（VideoPlayer 静音视频管线 appsink BGRx 软拷贝绘入播放区 + AudioPlayer 独立音频管线 S16LE/2ch/48kHz）+ 端到端时延统计（读取/拉出/提交/上屏四点）+ 音视频 PTS 漂移监测 + SIGTERM 优雅退出
- 布局：当前 tiling 分格自适应（480×325）；full_screen 占领 launcher 栏上方全部区域的应用侧代码已就绪，被 compositor 侧权限校验阻塞（见 §4.5 与 README §2.2）
- 目录：`/home/admin0412/windows_share/x9_linux/data/AndroidAuto/`
- 结构：`src/{app,model,view,wayland,util}` + `config/androidauto.ini`（[app]/[window]/[player]/[ui] 全参数化，禁硬编码）+ `res/{icons,shaders}` + `protocol/{ivi-application,ivi-hmi-controller}.xml` + Makefile + push.sh

## 2. 关键环境事实（host）

| 项 | 值 |
|---|---|
| SDK | `/home/admin0412/x9sp_wayland/`（core-image-weston 工具链，wayland-client 1.22 + cairo 1.18，依赖全齐，**优先用它**） |
| 备用 SDK | `/home/admin0412/x9sp/`（**缺 wayland 开发包**，勿用于本项目） |
| 原生 wayland-scanner | `/home/admin0412/x9sp_wayland/sysroots/x86_64-sdrvsdk-linux/usr/bin/wayland-scanner` |
| 协议 XML | `protocol/ivi-application.xml`（glue 构建时自动生成到 `obj/protocol/`） |
| weston 16 源码（参考用） | `../../weston/`（ivi-shell/hmi-controller.c 可对照，但**设备跑的是 weston 13 定制版**，行为有差异） |

## 3. 设备侧状态（x9m_ms）

| 项 | 状态 | 说明 |
|---|---|---|
| weston 13 + ivi-shell | ✅ 运行中 | `shell=ivi-shell.so` + 定制 `hmi-controller.so` + `weston-ivi-shell-user-interface` |
| launcher 图标（icon-id=4007） | ✅ 已显示 | `weston.ini` 追加了 AndroidAuto 条目（权威副本在**设备** `/data/weston.ini`，host 侧已同步） |
| app 点击启动 | ✅ 修复待最终确认 | 两个秒退 bug 已修（见 §5），等鼠标实测 |
| 开机自动（停动画+bind-mount+weston） | ✅ reboot 实测 3s 就绪 | drop-in 在设备 `/etc/systemd/system/weston.service.d/ivi.conf` |
| 触摸 | ❌ **内核无驱动** | `CONFIG_TOUCHSCREEN_SEMIDRIVE is not set`，需 BSP 换内核；**当前用 USB 鼠标**（已插，event0） |
| 部署路径 | — | `/data/AndroidAuto/{bin,config,res}`；日志 `/data/AndroidAuto/androidauto.log`（持久） |

## 4. 设备/工具怪癖清单（新会话必读，都踩过）

1. **wayland 1.22 SDK 回调签名与上游不同**：`axis_discrete`/`axis_value120` 无 time 参数——写 thunk 前先 `awk '/^struct wl_pointer_listener \{/,/^\};/' <sysroot>/include/wayland-client-protocol.h`
2. **libwayland 硬性行为**：`WAYLAND_SOCKET` 指向无效 fd 直接失败不回退；listener 结构体 NULL 回调直接 abort——app 里已做四级连接回退 + 全事件 no-op（`WlClient::connect`）
3. **定制 hmi-controller 有 assert 崩溃雷**：`weston.ini` 任一 `[ivi-launcher]` 的 icon 路径失效 → 客户端静默跳过该图标 → `add_launchers` assert → **整个 weston ABRT**。改 ini 后必须 `ls` 验证路径；图标已做双名称（`AndroidAuto.png`+`androidauto.png`）防大小写坑
4. **BusyBox 工具不可信**：`ps` 只列 tty 进程且 comm 截断 15 字符；`pgrep` 长 pattern 警告；`head -8` 不支持（用 `head -n 8`）；**无 timeout 命令**。查进程一律扫 `/proc/[0-9]*/cmdline`（按 comm 精确匹配，勿用 cmdline 通配——会匹配到 shell 自身自杀）
5. **设备无 RTC**：每次 reboot 时钟回到 2024-02-27 17:33，日志时间戳跨 boot 重叠——判断记录归属用 `uptime`
6. **dmesg 被刷爆**：触摸 rpmsg `no recipient`（190 端口）每秒多条冲掉启动日志，看启动问题要尽早 dump
7. **rootfs `/` 只读**：改 `/etc` 需 `mount -o remount,rw /` 改完 `remount,ro`；`/data` 是 init 脚本挂载（非 fstab），systemd 的 `RequiresMountsFor` 对它无效
8. **`/tmp` 是 tmpfs**：reboot 清空——持久日志/证据放 `/data`
9. **双屏 1920×720**（DPI-1/2）；app 收到的 configure 是 hmi-controller tiling 布局尺寸（480×325），已自适应
10. **VPU 只支持一路 h265 硬解**：两条管线播同一 mp4 时，音频管线的 uridecodebin 必须 `autoplug-continue` 拒绝视频轨（否则第二路 omxh265dec enable 失败拖死音频管线，实测）
11. **设备无 AAC 解码器**（gst-inspect 仅 aacparse）：mp4 AAC 音轨解不了；音频通路用 FLAC 验证（`/data/gstreamer_test/resource/jaychou_taojinxiaozhen.flac`）
12. **定 hmi-controller 有 bind 权限校验**：`bind_hmi_controller` 仅允许 weston-ivi-shell-user-interface 客户端，其余 bind 即 fatal 协议错误（`hmi-controller failed: permission denied`）。应用侧按 `fullscreen_layout=0` 规避（不绑）；反汇编确认单点 `b.eq`（0x29bc）可放行，但**不做二进制 patch，走厂商放行路线**（用户明确要求不改第三方库）

## 5. 已修复的关键 bug（详见 bug 文档）

### v1.1 播放功能验证结果（2026-09-16 21:00，全过）

| 用例 | 结果 | 实测数据 |
|---|---|---|
| A-1~A-3 构建 | ✅ | 0 error 0 warning；NEEDED 齐全（含 gst/glib） |
| B-3 启动存活 | ✅ | 命令行直启稳定运行，无秒退 |
| C2-1 双管线 | ✅ | prerolled → PLAYING，无 error |
| C2-2 视频画面 | ✅ | appsink BGRx 软拷贝绘入播放区，宽高比自适应（2:1） |
| C2-3 音频输出 | ✅ | ALSA card1 打开 S16_LE/2ch/48000（FLAC 源） |
| C2-4 首帧上屏 | ✅ | +477~645ms |
| C2-5 时延链路 | ✅ | pull→commit avg 7.4ms / commit→screen avg 10.2ms / e2e avg ~1s（含 1s 同步缓冲，read→pull 为主） |
| C2-7 EOS 循环 | ✅ | 视频 loop seek 正常 |
| C2-8 SIGTERM | ✅ | 优雅退出 + latency-summary（292 帧统计） |
| D-1 启停×10 | ✅ | 10/10 干净退出 |
| D-2 内存 5min | ✅ | VmData ~205MB 封顶（双管线 buffer pool 弹性水位 +16MB，非泄漏；单路各自 0 增长） |
| D-3 CPU | ✅ | 播放 ~176%（60fps NV12→BGRx 软转换预期开销） |

开发期排掉的真 bug：① 音频管线用 playbin flags=AUDIO 时 qtdemux not-linked（改 uridecodebin + fakesink 兜底）；② VPU 单路限制（autoplug-continue 拒视频轨）；③ 时延配账串帧（frameCbPending_ 期间不更新 pendingLat_）；④ SIGTERM 无 teardown（g_unix_signal_add）。

| 历史 bug | 根因一句话 | 修复提交 |
|---|---|---|
| app 点击秒退① | launcher spawn 链环境带失效的 `WAYLAND_SOCKET=38` + 错误的 `WAYLAND_DISPLAY=wayland-1`（实际 `/run/wayland-0`） | `2f61be5` |
| app 点击秒退② | pointer `frame` 等事件 listener 为 NULL → libwayland abort | `a768436` |
| weston 开机即崩 | ini icon 大小写不匹配 + hmi-controller assert | 设备侧修复（双名称副本） |
| reboot 黑屏 | /data 挂载晚于 weston 启动，ExecStartPre 127 | drop-in 改等待循环（设备侧） |
| reboot 丢 ivi 环境 | bind-mount 非持久 | drop-in 自动执行（设备侧） |

## 6. 快速命令速查

```bash
# 构建
cd /home/admin0412/windows_share/x9_linux/data/AndroidAuto
. /home/admin0412/x9sp_wayland/environment-setup-cortexa55-sdrv-linux
make && make check          # check = file + readelf 产物检查

# 部署（三种）
./push.sh                   # 推 bin/config/res + weston.ini + 重启 weston
./push.sh --no-restart      # 只推文件（改 ini 后 app 重开即生效，不用动 weston）
./push.sh <serial>          # 指定 adb 设备

# 设备侧排障
adb shell "cat /data/AndroidAuto/androidauto.log"        # app 日志（触点坐标/生命周期）
adb shell "journalctl _COMM=weston --no-pager | tail -20" # weston 日志（assert 看这里）
adb shell "systemctl is-active weston"                   # 服务状态
adb shell "XDG_RUNTIME_DIR=/run WAYLAND_DISPLAY=wayland-0 setsid \
  /data/AndroidAuto/bin/AndroidAuto &"                   # 命令行直启（绕过 launcher）
# 用 launcher 真实环境复现 spawn 问题：读 /proc/<launcher_pid>/environ
```

## 7. 待办事项（按优先级）

1. **full_screen 布局放行**（阻塞中，BSP/厂商范围）：应用侧已就绪（switchLayout + 协议 glue + ini 开关），等 hmi-controller bind 权限放行后 `fullscreen_layout=1` 即可；材料见 README §2.2
2. **AAC 解码器**（BSP 范围）：补插件后 audio_uri 可换回 mp4 同源播放（漂移监测才有意义）
3. **端到端最终验证**（等用户鼠标实测）：launcher 点击启动 → 播放/声音 → ✕ 关闭 → 反复 10 次
4. v1.1 改动 git 提交（用户确认后）
5. 触摸驱动（BSP 范围，`CONFIG_TOUCHSCREEN_SEMIDRIVE=y` 内核）

## 8. git 历史

```
ebbb3de 排查沉淀：ivi 集成期五故障复盘
a768436 修复秒退二连：WAYLAND_SOCKET/WAYLAND_DISPLAY 环境 + listener NULL abort
2f61be5 修复 launcher 启动秒退：wayland 连接回退 /run
88c2abb README: 部署章节改用 push.sh
205116f push.sh: 只推送运行时必需文件
922e953 AndroidAuto v1.0: ivi-shell wayland client (C++17/MVC)
```
