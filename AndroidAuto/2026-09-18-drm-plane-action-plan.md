# DRM Plane 混合渲染路线行动书

> 项目：AndroidAuto（x9m_ms / Weston 13 ivi-shell / GStreamer 1.22 / SemiDrive X9HP）
> 路线：**混合方案（路线 C）**——weston 保留 launcher/启动/UI/输入；应用打开后视频经自控 DRM plane 直扫上屏；视频区点击坐标经 evdev 直读回报。
> 性质：行动书（**尚未开发**）。前置调研与实测证据见 `2026-09-18-dmabuf-zero-copy-route.md`。
> 日期：2026-09-18

---

## 1. 目标架构

```
launcher 点击（weston ivi launcher）
  → start.sh → 应用进程（单进程，双通路）
     ├── wayland 通路（保留现状）
     │     WlClient/ivi_surface → 标题栏/关闭按钮/占位 UI（wl_shm + cairo）
     │     格子内输入走 weston seat（wl_pointer/wl_touch → InputController）
     └── DRM 通路（新增）
           omxh265dec use-dmabuf=true → appsink(memory:DMABuf NV12)
           → drmModeAddFB2WithModifiers → 空闲 YUV overlay plane 直扫
           dest rect = 播放区屏幕坐标（与 wayland 窗口内播放区对齐）
     └── evdev 输入（新增）
           /dev/input/event* 多读者直读（与 weston 并行，互不抢夺）
           → 坐标映射视频 dest rect → 命中 → 回调上报（投屏协议层/应用语义）
```

设计规则（均有实测依据，详见 `2026-09-18-dmabuf-zero-copy-route.md` §7/§8）：

1. 选 plane 前查 `FB_ID`/`CRTC_ID` property，非零（weston 占用）即跳过；
2. 只 setplane overlay，不 modeset、不碰 primary；
3. 退出只关自己的 plane（setplane fb=0）+ 对 ivi_surface commit 一帧 damage 触发 weston 重绘；
4. 帧缓冲生命周期：buffer 归还 pool 前确认 plane 已换下（双 fb 轮换或 fence）；
5. modifier 透传（AddFB2WithModifiers），plane 能力 property 枚举匹配。

---

## 2. 现状代码盘点（对路线 C 的可用性）

| 模块 | 现状 | 路线 C 下角色 | 复用度 |
|---|---|---|---|
| `src/wayland/WlClient.*` | 连接/ivi_surface/seat 输入/hmi-controller 按需绑 | 原样保留（UI 面输入 + ivi_surface） | ✅ 全复用 |
| `src/wayland/ShmBuffer.*` | wl_shm 双缓冲 + cairo surface | 保留（UI 面仍走 shm） | ✅ 全复用 |
| `src/view/AppView.*` / `Layout.*` | cairo 绘制：标题栏/关闭按钮/**视频帧绘制**/占位 | UI 部分复用；**视频帧 cairo 绘制路径在 DRM 模式下停用**（视频在硬件 plane，UI 只画边框/底色/占位）；`Layout::videoWindow` 几何成为**视频 dest rect 的来源** | ⚠️ 部分复用 |
| `src/media/MediaPlayer.*` | 统一管线；视频链 `appsrc→queue→h265parse→omxh265dec→videoconvert(BGRx)→appsink`；pullVideoFrame 返回内存帧 | **改动核心**：DRM 模式去掉 videoconvert+capsfilter，appsink 协商 `memory:DMABuf`，帧输出从"内存指针"变"fd+modifier 描述"；音频链/包接口/纠偏/卡顿检测全部不动 | ⚠️ 改造 |
| `src/media/MediaTypes.h` | `VideoFrame{data,width,height,stride,ptsNs}` | 扩展：dmabuf 描述（fd/fourcc/modifier/plane offset+pitch），与 data 指针二选一 | ⚠️ 扩展 |
| `src/media/FilePacketFeeder.*` | 文件→包桥 + 1x 重放 + 循环重建 | 原样复用（节奏机制与渲染通路无关） | ✅ 全复用 |
| `src/app/AndroidAutoApp.*` | glib 主循环/装配/时延统计（四点链路） | 主循环复用（新增 DRM fd、evdev fd 两个源）；**时延统计链路改点**：commit→上屏点对 DRM 路径不适用（无 wl_surface_frame），改用 plane 换帧确认（双 fb 轮换时刻或 out-fence） | ⚠️ 改造 |
| `src/app/InputController.*` | weston 输入→关闭语义 | 原样复用（格子内 UI）；视频区坐标上报为**新增独立通路** | ✅ 全复用 |
| `src/util/UsbDeviceMonitor.*` / `Log.h` | USB 监控/RAII 日志 | 原样复用 | ✅ 全复用 |
| `config/androidauto.ini` | 全参数 | 新增 `[player] render_path = shm|drm`（默认 shm，保底回退） | ⚠️ 扩展 |
| `Makefile` | PKGS 无 libdrm/libinput | 新增 `libdrm`（evdev 裸读不需要新库；libinput 视触摸阶段再定） | ⚠️ 扩展 |
| `push.sh` / `start.sh` | 部署/启动 | 原样（root 权限已满足 /dev/dri、/dev/input 访问） | ✅ 全复用 |

**新增模块（本路线的全部新代码量）**：

| 模块（拟定） | 职责 | 参考实现 |
|---|---|---|
| `src/drm/DrmPlaneCtl.*` | 打开 /dev/dri → 枚举 plane（占用+能力）→ 选空闲 YUV overlay → AddFB2WithModifiers → setplane（legacy 起步，atomic+fence 可选）→ 双 fb 轮换 → 退出清理 | `display-test/display/DrmDisplay.cpp`（`choose_perfect_plane`/`setPlane`/`addFrameBuffer`/`PostAsync`） |
| `src/input/EvdevReader.*` | 打开 /dev/input/event*（多读者）→ 挂 glib 主循环 → ABS(触摸)/REL(鼠标) 解析 → 屏幕坐标 → 视频 dest rect 命中 → 坐标回调上报 | 新写（无现成参考；触摸可换 libinput） |

---

## 3. 行动阶段（按依赖顺序，含验收标准）

### 阶段 0：验证实验（不改任何代码，全是命令行）

| # | 实验 | 命令/方法 | 通过判据 |
|---|---|---|---|
| 0-1 | appsink 类消费者能协商 `memory:DMABuf` | `gst-launch-1.0 filesrc location=h265_aac_1920x960_60fps.mp4 ! qtdemux name=d d.video_0 ! queue ! h265parse ! omxh265dec use-dmabuf=true ! fakesink dump=TRUE num-buffers=1 -v` | `-v` 输出 caps 含 `video/x-raw(memory:DMABuf)` |
| 0-2 | weston 运行中枚举 plane 占用与能力 | 小工具或 `modetest`（若板上有）：列 plane 的 FB_ID/CRTC_ID/format/modifier/zpos | 确认 plane0/1（YUV）中至少一个 FB_ID=0（空闲）；记录 weston 占用的 plane id（应为 43） |
| 0-3 | evdev 多读者与 weston 并行 | 应用侧临时 strace/cat 验证：两个进程同时读 `/dev/input/event*` 均收到完整事件流 | 双方事件流完整、互不干扰 |
| 0-4 | （低优先）确认应用 ivi surface 的**屏幕绝对坐标** | 播放期截图/光标位置对照，或 weston 日志 | 得到"格子位置→屏幕坐标"映射依据（见 §5 风险 R1） |

### 阶段 1：DRM plane 控制（`src/drm/DrmPlaneCtl.*`）

1. 打开 `/dev/dri/card*`，`drmModePlaneRes` 枚举，property 读 FB_ID/CRTC_ID 过滤占用（规则 1）；
2. plane format/modifier 能力匹配（`supportThisFormat` 式 + modifier 位图）；
3. `drmModeAddFB2WithModifiers`（fd/pitch/offset/modifier 来自 GstBuffer 的 dmabuf memory + GstVideoMeta）；
4. `drmModeSetPlane`（legacy）挂 overlay，dest rect 参数化（规则 2）；
5. 双 fb 轮换：保持 2 个 fb 在 plane 交替，旧 buffer 归还（规则 4 起步版，fence 后续优化）；
6. 清理：setplane(fb=0) 只关自己（规则 3 前半）。

**验收**：单元级——appsink dmabuf 帧能挂上空闲 plane 屏幕出画；退出后 weston 界面无损（无需鼠标唤醒即恢复或 damage 后恢复）。

### 阶段 2：MediaPlayer 视频通路改造

1. `Options`/ini 增加 `render_path`（shm|drm，默认 shm）；
2. DRM 模式：视频 bin 去掉 `videoconvert`+BGRx capsfilter，appsink caps 放开到 `video/x-raw(memory:DMABuf)`；
3. `pullVideoFrame` 输出扩展：`VideoFrame` 增加 dmabuf 描述字段（fd、fourcc=NV12、modifier、每 plane offset/pitch），DRM 模式下 data 指针为空；
4. 保持 appsink `max-buffers=2, drop=TRUE` 现状与双 fb 轮换配套（帧持有窗口语义与现在一致）；
5. 音频 bin、包接口、Feeder、纠偏、卡顿检测**零改动**。

**验收**：文件源 60fps 播放，视频经 plane 上屏，CPU user 时间相对 shm 路径显著下降（对照 §4 实测量级：16.6s 播放 user < 1s）；循环播放（管线重建）下 plane 路径正常轮换。

### 阶段 3：装配与 UI 适配

1. `AndroidAutoApp`：`render_path=drm` 时不把 VideoFrame 交给 cairo 绘制，改为调 DrmPlaneCtl 提交；UI 面照常重绘（标题栏/边框/占位）；
2. dest rect 计算：`Layout::videoWindow`（窗口局部坐标）+ 应用 ivi surface 屏幕原点（阶段 0-4 结论）→ 屏幕坐标；
3. 停止播放/退出：关 plane + 对 ivi_surface commit 一帧 damage（规则 3 后半）；
4. 时延统计：DRM 路径的"上屏点"改用双 fb 轮换确认时刻（或 atomic out-fence，后续优化），`[latency]` 日志字段语义更新。

**验收**：launcher 启动→播放→✕ 关闭全流程，weston 界面任何时点无损；TEST_PLAN A/B 组回归。

### 阶段 4：evdev 输入与坐标上报

1. `EvdevReader`：open event 节点（先鼠标 REL；触摸 ABS 结构预留），fd 挂 glib 主循环；
2. 鼠标 REL 累积跟踪屏幕坐标（与 weston 光标同一事件流，理论一致）；
3. 坐标 ∈ 视频 dest rect 判定，命中即触发坐标回调（接口预留给投屏协议层）；
4. 触摸（等 BSP 内核 `CONFIG_TOUCHSCREEN_SEMIDRIVE`）：届时评估 libinput 替换裸 evdev（slot/协议 B 处理），坐标映射做一次校准。

**验收**：鼠标点击视频区任意点，日志/回调输出屏幕坐标与 dest rect 内相对坐标；点击 UI 格子内仍走原 InputController 路径互不串扰。

### 阶段 5：回归与性能基线

1. TEST_PLAN 回归集：A-1~A-3、B-3、C-2、C-3、C-5、C-9、D-1（现有最小回归集）；
2. D-2 内存复测（双 fb 轮换引入的固定 fb 引用是否影响 pool 封顶行为）；
3. D-4 与 weston-simple-egl 同开（plane 层级/zpos 冲突验证）；
4. 性能采样：CPU（对照 ~176% 基线）、丢帧、[latency] 链路（改造后语义）；
5. 更新 TEST_PLAN.md / README.md / CHECKPOINT。

---

## 4. 与 BSP/厂商的并行事项（不阻塞本路线，需跟踪）

| 事项 | 影响 |
|---|---|
| libgstwayland viewporter v3 bind bug 修复 | 路线 A（weston 内 dmabuf）验证入口；修复后可做路线 A 实测，作为长期演进形态评估 |
| hmi-controller bind 权限放行 | full_screen 布局；若放行，视频 dest rect 可扩到全应用区且 UI surface 覆盖视频区，点击可回 wayland 输入 |
| 触摸内核（CONFIG_TOUCHSCREEN_SEMIDRIVE） | 阶段 4 触摸部分的前置 |

---

## 5. 风险与开放问题

| # | 风险/问题 | 影响 | 缓解 |
|---|---|---|---|
| R1 | **应用 ivi surface 屏幕绝对坐标未知**（configure 只给尺寸；tiling 格子位置是 compositor 决定的） | dest rect 若映射错，视频画面位置与 UI 播放区错位 | 阶段 0-4 实测确定映射；tiling 下应用固定左上格，坐标大概率是常量，写进 ini 兜底可调 |
| R2 | weston 在"被视频盖住的区域"仍按自己的 surface 树派发输入 | 点击视频区可能误触发被盖住的 launcher 图标等 weston 侧行为 | 实测评估；必要时与 BSP 沟通（或等 R-hmi 放行后 surface 全屏覆盖解决） |
| R3 | 双 fb 轮换与 GStreamer buffer pool 生命周期 | 过早归还 buffer 会被 plane 扫描已释放内存（花屏/崩溃） | 阶段 1 先保守（持有至下一帧提交），观察 D-2；必要时 atomic out-fence |
| R4 | TILED modifier 与 plane 能力匹配 | AddFB2WithModifiers 可能被拒 | kmssink 实测已证 semidrive plane 吃 VPU 输出（§4/§8）；阶段 0-2 枚举能力确认 |
| R5 | 鼠标 REL 双读者坐标漂移（weston 与应用各自累积） | 应用侧命中判断偏差 | 同一事件流同一初值，理论无漂移；阶段 0-3 验证 |
| R6 | drmModeSetPlane legacy API 与 weston atomic 提交并存 | 极端时序下 plane 更新竞争 | 本场景双方更新频率低（视频 60fps vs UI 偶发），实测观察；异常再升 atomic |
| R7 | 循环播放整管线重建期间 plane 无 fb | 轮切换帧间隙视频区闪烁 | 保留上一 fb 至新会话首帧提交后再释放（与现有 ~400ms 首帧间隔配合） |

---

## 6. 交付物清单（完成后）

1. `src/drm/DrmPlaneCtl.*`、`src/input/EvdevReader.*` 新模块；
2. `MediaPlayer` render_path 双模式（shm 保底回退，ini 一键切回）；
3. `androidauto.ini` 新增 `[player] render_path` 及 dest rect 校准参数；
4. `Makefile` 增 libdrm；
5. TEST_PLAN 增 D 组 DRM 路径用例；README/CHECKPOINT 同步；
6. 性能对比数据（CPU/丢帧/时延，shm vs drm 双路径同素材）。

---

## 附：执行前置

- 构建环境：`. /home/admin0412/x9sp_wayland/environment-setup-cortexa55-sdrv-linux && make`（阶段 1 起需确认 sysroot 含 libdrm 开发包）；
- 部署：`./push.sh` 不变；
- 阶段 0 的四项实验建议先行，全部通过后再进入阶段 1。
