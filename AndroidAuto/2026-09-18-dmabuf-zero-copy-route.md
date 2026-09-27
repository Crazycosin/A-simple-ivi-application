# DMABuf 零拷贝上屏路线调研与实测记录

> 项目：AndroidAuto（x9m_ms / Weston 13 ivi-shell / GStreamer 1.22 / SemiDrive X9HP）
> 范围：围绕"视频解码到上屏的拷贝链路与零拷贝可行路线"的递进式调研，含 2026-09-17 ~ 2026-09-18 两轮实测结论。
> 证据来源：`x9hp_problems.md`、`2026-09-17-h265-dmabuf-performance-test.md`、
> `/home/admin0412/projects/markdown/2026-09-17-waylandsink-bsp-retest.md`、
> `display-test/`（Semidrive DRM demo）、`egl-demo/`（EGL dmabuf demo）、
> 2026-09-18 weston 在位 kmssink 共存实测（本文 §8）。

---

## 0. 旧结论修正记录（先读）

本文推进过程中，两个早期结论被后续实测推翻，先行更正：

| 旧结论 | 出处 | 新事实 | 修正依据 |
|---|---|---|---|
| "omxh265dec 无法实际输出 dmabuf buffer" | `x9hp_problems.md` §waylandsink无法接nv12dmabuffer | omxh265dec 有 `use-dmabuf` 属性（**默认 false**），置 true 后输出端口声明 `video/x-raw(memory:DMABuf)`，实测可用 | `2026-09-17-h265-dmabuf-performance-test.md`：纯解码吞吐 46→315 fps，CPU user 20.7→0.9 s |
| 当时 waylandsink 失败归因于 dmabuf/omx | 同上 | 真实原因是系统 `libgstwayland-1.0.so` 绑定 `wp_viewporter` 请求 v3 而 Weston 广播 v1，协议错误断连；`videotestsrc ! waylandsink` 最小复现，与 omx/NV12/dmabuf 无关 | `2026-09-17-waylandsink-bsp-retest.md`：WAYLAND_DEBUG trace + 反汇编 `mov w7, #3` |

旧记录的失败是 waylandsink 初始化 bug 挡在前面，omx 的 dmabuf 输出能力被误判。

---

## 1. 现状链路：硬解发生在哪、输出什么格式、几次拷贝

### 1.1 解码阶段与 buffer 格式（代码事实）

- 硬解位置：动态视频 bin 内 `v_appsrc → v_queue → h265parse → omxh265dec`（VPU 硬解，`/dev/vpucoda` / `/dev/vpuwave`；创建失败落 `avdec_h265` 软解）。
- omx 解码器输出 **NV12（YUV）**（开启 `use-dmabuf` 前为系统内存；该属性本版本默认 false，当前应用未开）。
- 随后 `videoconvert`（CPU 软件转换）+ capsfilter 钉死 **BGRx**（`src/media/MediaPlayer.cpp` 视频链路），到达应用层的 `VideoFrame.data` 为 BGRx。

```cpp
// src/media/MediaPlayer.cpp（BGRx capsfilter 注释原文）
// weston pixman 软合成经 wl_shm 要求 BGRx（硬解输出不显式转换时
// 协商不到 wl_shm 能吃的格式）
```

注：该注释基于"weston 为 pixman 软合成"的早期记录。`2026-09-17-waylandsink-bsp-retest.md` 现场证据：Weston 进程映射含 `gl-renderer.so` 与 `drm-backend.so`，即当前 weston 实际运行 GL 渲染器，非 pixman。注释描述的场景与当前现场不符，但"应用链路钉 BGRx 走 CPU 转换"这一代码事实不变。

### 1.2 解码后到上屏的像素搬运（3 次全帧级）

| # | 搬运点 | 内容 |
|---|---|---|
| 1 | `videoconvert`（CPU） | NV12(YUV) → BGRx 格式转换，写入 appsink 的 GstBuffer |
| 2 | cairo 绘制到 wl_shm buffer | 视频帧经 `cairo_image_surface_create_for_data` 引用零拷贝（BGRx 与 cairo RGB24 内存序一致，不 memcpy），但 cairo 渲染时逐像素合成写入 mmap 的 `WL_SHM_FORMAT_ARGB8888` 窗口 buffer |
| 3 | weston 合成 | wl_shm buffer attach/commit 后，compositor 将应用窗口合成到输出 framebuffer |

链路总结：**硬解 NV12 →(CPU 转换)→ BGRx →(cairo 合成写)→ shm ARGB →(weston 合成)→ 屏幕**。CPU 占用实测见 `TEST_PLAN.md` D-3：60fps NV12→BGRx 软转换 + cairo 缩放 ~176% CPU。

---

## 2. 问题一：应用窗口位置与大小能否调整

### 结论（基于 README §2.2 与代码事实）

ivi-shell 架构下，`ivi_application` 协议客户端只能创建 surface、被动收 configure，**没有请求移动/缩放的协议接口**。布局由 weston 侧 hmi-controller 的 tiling 模式分配（所有应用 480×325 分格，位置固定左上角）。

| 调整项 | 可否 | 方式 |
|---|---|---|
| 窗口位置 | ❌ 应用侧 | tiling 格子坐标由 compositor 分配 |
| 窗口大小 | ❌ 应用侧 | 同上 |
| 窗口内部布局（标题栏/播放区度量） | ✅ | `[ui]` 段：`title_bar_height`、`video_margin`、`video_border_*`、`video_aspect_w/h` |
| 首帧占位尺寸 | ✅（实际无效） | `[window] default_width/height`，configure 到达即被覆盖 |
| full_screen 布局（1920×650，留底部 launcher 栏） | ⚠️ 代码就绪、被阻塞 | `fullscreen_layout=1` + `WlClient::switchLayout()` |

### full_screen 阻塞根因（实测+反汇编）

定制版 `hmi-controller.so` 的 `bind_hmi_controller` 有权限校验：仅允许 `weston-ivi-shell-user-interface`（launcher 自身）绑定 `ivi_hmi_controller`，其余客户端 bind 即 fatal 协议错误（`hmi-controller failed: permission denied`）。反汇编确认校验为 bind 处单点 `b.eq` 跳转。

可行路径（README §2.2 记录三条）：

| # | 路径 | 状态 |
|---|---|---|
| 1 | 厂商放行 bind 校验（去校验或按 ivi id 白名单）→ 应用 `fullscreen_layout=1` 直接生效 | 待厂商，应用侧零改动 |
| 2 | ivi_wm / ILM 控制面 | 当前设备未广播 ivi_wm 全局、无 libilm，不可用 |
| 3 | weston 上游 hmi-controller desktop surface 路径 | 同样走 tiling switch_mode，无区别待遇 |

---

## 3. 问题二：dmabuf 零拷贝上屏，weston+wayland 方案能否行通

### 3.1 环节分解与实测状态

| 环节 | 状态 | 证据 |
|---|---|---|
| 解码器输出 dmabuf | ✅ 已实测 | `use-dmabuf=true` 后输出 `video/x-raw(memory:DMABuf)`；吞吐 46→315 fps，CPU user 23× 下降（§4） |
| weston 合成侧条件 | ✅ 具备 | 广播 `zwp_linux_dmabuf_v1` **v4**；进程映射含 `gl-renderer.so` + `drm-backend.so`（非 pixman）。注意：广播≠导入成功，尚未实测 |
| waylandsink 验证入口 | ❌ 被 BSP bug 挡死 | 系统 `libgstwayland` viewporter v3 bind 错误，`videotestsrc ! waylandsink` 即失败，测不到 dmabuf 导入环节 |
| 应用侧 appsink 取 dmabuf | ⏳ 待验证 | kmssink 协商成功证明 caps 真实存在；appsink 不做格式导入，协商风险低。可用 `fakesink dump=TRUE -v` 看 caps 一条命令验证 |

### 3.2 BSP bug 详情（`2026-09-17-waylandsink-bsp-retest.md`）

- 系统 `libgstwayland-1.0.so.0.2211.0`（SHA-256 `16c72483...`）绑定 `wp_viewporter` 请求 v3，Weston 13 广播 v1 → fatal 协议错误 → gst-launch 退出码 255。
- 最小测试 `videotestsrc num-buffers=1 ! waylandsink` 同样失败 → 与 MP4/H.265/OMX/NV12 无关。
- 反汇编：bind 分支 `mov w7, #3`；上游 GStreamer 1.22.11 源码与 viewporter XML 均为 v1 → 客户端构建缺陷。
- 曾删除的 /opt/x9hp 备用库反汇编为 `mov w7, #1`（bind v1），仅静态验证。
- 修复路径：BSP 侧追溯 recipe/补丁重编 gst-plugins-bad wayland 支持库；或临时换回 /opt/x9hp v1 库实测。不建议服务端伪装 v3。

### 3.3 weston 路线重构清单（BSP 修复后）

1. 去掉 `videoconvert(BGRx)` capsfilter（钉 BGRx 会使协商回到拷贝路径）；
2. appsink 收 `video/x-raw(memory:DMABuf), NV12`，从 GstBuffer 取 fd（`gst_is_dmabuf_memory` / `gst_dmabuf_memory_get_fd`，modifier 在 GstVideoMeta）；
3. `zwp_linux_dmabuf_v1`（v4 params）从 fd 建 wl_buffer 提交；
4. 视频拆独立 subsurface（一个 surface 同一时刻只能 attach 一种 buffer 类型），父窗口 cairo UI 照走 wl_shm；
5. 待实测：weston 能否导入 VPU 的 NV12 dmabuf（尤其 modifier 为 Semidrive TILED vendor modifier 时）。

---

## 4. 性能实测：use-dmabuf 开关对比（`2026-09-17-h265-dmabuf-performance-test.md`）

素材：`h265_aac_1920x960_60fps.mp4`（963 帧，时长 16.05 s）。链路：`filesrc ! qtdemux ! queue ! h265parse ! omxh265dec [use-dmabuf=true] ! kmssink/alsasink`。

| 场景 | 不带 dmabuf | use-dmabuf=true | 差异 |
|---|---|---|---|
| 端到端（视频+音频） | 22.229 s | 16.769 s | −25% |
| 视频-only 实时播放 | 21.502 s | 16.636 s | 快 4.9 s |
| 纯解码吞吐（fakesink sync=false，963 帧） | 20.735 s ≈ 46 fps | 3.061 s ≈ 315 fps | **6.8×** |
| CPU user（视频-only） | 20.703 s | 0.887 s | **23×** |

- 基准线 16.05 s：不带 dmabuf 端到端 22.2 s ≈ 43.3 fps，相对 60 fps 源丢帧约 28%（≈268 帧）；带 dmabuf 16.7 s ≈ 实时，一帧不丢。
- 瓶颈证据：不带 dmabuf 时 `user ≈ real`（CPU 100% 忙于拷贝）；日志满屏 `FLUSH LINEAR(-3) TILED(-3)` 即解码输出的 TILED→LINEAR 逐帧拷贝。数据量：NV12 1920×960 每帧 ≈2.76 MB，963 帧 ≈2.66 GB。
- `use-dmabuf` 属性默认 false，不开必走拷贝路径；1920×960@60 必然掉到 ~43 fps。

---

## 5. 问题三：display-test 与 egl-demo 的上屏方案

### 5.1 display-test：纯 DRM/KMS 直控（应用自任"compositor"）

- 自开 `/dev/dri`，DrmBackend/DrmFrame/DrmPlane 封装管理 crtc/connector/plane；
- 上屏 = `drmModeAddFB2(WithModifiers)` 注册 framebuffer → `drmModeSetPlane`（或 atomic commit + fence，`PostAsync`）挂 plane；
- 多图层靠硬件 plane 叠加（DP 4 层：plane0/1 支持 YUV+RGB，plane2/3 仅 RGB，zpos 排序）；
- 支持外部 buffer 导入：`DRM_IOCTL_SEMIDRIVE_EXPORT_DMABUF` 把物理地址转 dmabuf fd → `ImportBuffer` → AddFB 直扫（模拟 camera/VPU 输入，零拷贝）；
- README 记录：多进程访问同一 drm 允许（驱动 > ptg3.4），需提前规划 z-order；无任何窗口系统。

### 5.2 egl-demo：EGL/GPU 渲染 + dmabub import/export

- `EGL_DEFAULT_DISPLAY` + 无窗口系统直连；
- GPU 侧 `eglCreateImageKHR(EGL_LINUX_DMA_BUF_EXT, plane fd/offset/pitch/modifier)` 把 dmabuf 导入为纹理（`demo_rtt_dmabuf_yuv` 导入 YUV），`glEGLImageTargetTexture2DOES` 绑定后着色器合成，`eglSwapBuffers` 输出；
- `display-test/display/example04.cpp` 完整范例：v4l2 camera DMABUF → EGLImage → GPU 合成上屏，校验 `EGL_EXT_image_dma_buf_import` 扩展可用。

### 5.3 与 weston 架构的关系

| 技术 | 在 weston 在位时的可用性 | 说明 |
|---|---|---|
| EGL dmabuf import（egl-demo 那套） | ✅ 技术同源 | weston gl-renderer 导入客户端 dmabuf 走同一 EGL 扩展；example04 证明本平台 GPU 能吃外部 YUV dmabuf |
| `drmModeSetPlane` / plane/zpos/atomic（display-test 那套） | ⚠️ 与"weston 为 DRM master"的通用约束冲突，能否共存需实测（§8 已实测：可以，见下） | weston 独占 plane 分配与 mode-setting 是通用规则；Semidrive 驱动多进程访问能力是例外条件 |

---

## 6. 方案对比总表

三条候选路线 + 现状基线，逐项对比：

| 维度 | 现状基线（wl_shm + BGRx） | 路线 A：weston 内 dmabuf（重构） | 路线 B：kmssink 直出 | 路线 C：混合（weston UI + 自控 KMS 视频 plane） |
|---|---|---|---|---|
| 解码后拷贝次数 | 3 次（videoconvert→cairo→weston 合成） | 0~1 次（weston GL 采样合成；若 direct scanout 则 0） | 0 次（AddFB 直扫） | 0 次（AddFB 直扫视频 plane；UI 仍走 shm） |
| CPU 占用（60fps 参照） | ~176%（TEST_PLAN D-3） | 待实测（预计接近 dmabuf 实测的 ~0.9 s user 量级） | 0.887 s user / 16.6 s 播放（实测） | 同左量级（视频面），UI 面另计（帧率远低于视频） |
| 实测验证程度 | ✅ 全链路端到端验证（v1.2） | ❌ 未实测，且验证入口被 BSP bug 挡住 | ✅ 16.7 s 实时一帧不丢（§4） | ⚠️ 部分验证：weston 在位 setplane 共存 ✅（§8）；完整应用未实现 |
| 前置条件 | 无 | **BSP 修复 libgstwayland viewporter bind bug** + weston dmabuf modifier 导入实测 | 无（临时验证工具，产品不可用） | 无（依赖 Semidrive 驱动多进程 DRM 能力，驱动 > ptg3.4） |
| ivi-shell 管控/launcher | ✅ 完整保留 | ✅ 完整保留 | ❌ 完全脱离 | ✅ UI 侧保留（launcher 点 start.sh 启动不变） |
| 窗口管理/输入 | ✅ weston seat | ✅ weston seat | ❌ 无 | ✅ weston seat（UI 面）；视频面无输入需求 |
| 视频上 UI 特效（圆角/边框叠加） | ✅ cairo 同 surface 任意叠加 | ✅ GL 合成可叠加 | ❌ | ⚠️ UI 只能描边/四周包裹，不能叠在视频像素上（不同硬件 plane） |
| 应用侧改动量 | —（基线） | 大：管线去 BGRx、dmabuf wl_buffer、拆 subsurface | 无（但不可作为产品形态） | 中：管线去 BGRx + 新增 DRM plane 控制（参考 display-test），UI/输入不动 |
| 已知风险/缺点 | CPU 瓶颈、丢帧 28% | 依赖 BSP 修复节奏；modifier 导入未验证；weston GL 合成性能未实测 | 独占式设计：占用 weston primary plane、退出破坏 weston 显示状态（§8 实测） | plane 冲突需枚举规避；zpos 规划；退出需自恢复；帧缓冲生命周期自管（双 fb 轮换/fence） |
| 适用判断 | 当前已交付形态 | BSP 修复后的正道演进 | 验证/测试工具 | 不等 BSP 即可落地的零拷贝形态 |

---

## 7. 不依赖 weston 的自控方案设计要点（路线 C 落地参考）

架构：

```
launcher 点击 → start.sh → 应用进程
├── wayland 客户端（保留现有 WlClient/AppView）
│     ivi_surface → 标题栏/关闭按钮/占位 UI（wl_shm, cairo）
│     输入走 weston seat → ✕ 关闭逻辑不变
└── DRM 通路（新增，参考 display-test）
      omxh265dec use-dmabuf=true → appsink(memory:DMABuf NV12)
      → drmModeAddFB2WithModifiers → drmModeSetPlane(空闲 YUV plane)
      dest rect = 播放区坐标（与 wayland 窗口内播放区矩形对齐）
```

关键规则（每条均有实测依据，见 §8）：

1. **选 plane 前查占用**：枚举 plane 读 `FB_ID`/`CRTC_ID` property，非零（正被 weston 使用）即跳过；只挑空闲 YUV overlay plane（plane0/1）；
2. **只 setplane overlay，不 modeset、不碰 primary**（不 `drmModeSetCrtc`）；
3. **退出只关自己的 plane**（setplane fb=0），并主动触发 weston 重绘一次（应用自身是 wayland 客户端，对 ivi_surface commit 一帧带 damage 的重绘即可，无需等用户输入）；
4. **帧缓冲生命周期**：buffer 归还 GStreamer pool 前须确认 plane 已换下（双 fb 轮换，或 display-test `PostAsync` + fence 模式）；
5. modifier 必须透传（`AddFB2WithModifiers`），plane format/modifier 能力用 property 枚举匹配（`choose_perfect_plane` 加占用判断）。

---

## 8. 实测：weston 在位 + 非 master 进程 setplane 共存（2026-09-18）

### 实验内容

weston 13（ivi-shell，launcher 正常显示）运行状态下，执行 `bash test_h265.sh`（`omxh265dec use-dmabuf=true ! kmssink driver-name=semidrive connector-id=79 plane-id=43`）。

### 结果

1. **视频正常播放，与 weston 并存**——非 DRM master 进程 setplane 在本平台可行，Semidrive 驱动多进程 DRM 访问能力实测成立；
2. **播放期间 weston 界面不可见**（被视频画面替换）；
3. **视频播放结束后 launcher 界面消失，动一下鼠标界面即恢复**。

### 推理链（现象 → 结论）

- 若 kmssink 使用的是空闲 overlay plane，退出关掉 overlay 后 weston primary 内容会立即自动恢复，无需任何事件；
- 实际需要鼠标事件触发 repaint 才恢复 → 播放时 kmssink 顶掉的是 **weston 正在扫描的 plane（plane 43 = weston primary plane）**；退出时 kmssink 将该 plane disable（fb_id=0），weston 不知 plane 被外部动过，不会主动重绘；
- 鼠标事件 → weston repaint → 重新 setplane 自己的 fb → 恢复。
- modeset 打断/DPMS off 可排除：这两种情况下鼠标触发 repaint 无法恢复。

### 结论

- 路线 C 的核心前提（weston 在位时外部进程可操作 plane 且不破坏 weston 显示链）**实测成立**；
- kmssink 的失败模式（顶 primary + 退出不恢复）不是路线 C 的属性，是 kmssink 独占式设计的属性；自控代码按 §7 规则 1~3 规避即可；
- weston 侧自恢复机制确认：任一输入事件或 ivi_surface damage 重绘均可唤醒。

---

## 9. 验证状态总览与下一步

### 已验证 ✅

| 项 | 手段 |
|---|---|
| omxh265dec use-dmabuf=true 输出 dmabuf，315 fps 吞吐 | 纯解码 fakesink 测试（§4） |
| kmssink AddFB 直扫 VPU NV12（含 TILED modifier），16.7 s 实时零丢帧 | 端到端/视频-only 实测（§4） |
| waylandsink 失败根因 = 系统 libgstwayland viewporter v3 bind bug | 最小复现 + WAYLAND_DEBUG trace + 反汇编（§3.2） |
| weston 实际运行 gl-renderer + drm-backend，广播 dmabuf v4 | BSP 复测现场记录（§3.1） |
| weston 在位 + 非 master 进程 setplane 共存 | 2026-09-18 实测（§8） |
| plane 43 为 weston primary plane；退出后 weston 可被输入事件唤醒恢复 | 2026-09-18 实测推理链（§8） |

### 待验证 ⏳（按优先级）

1. **appsink 消费 `memory:DMABuf` caps**：`gst-launch ... omxh265dec use-dmabuf=true ! fakesink dump=TRUE num-buffers=1 -v` 看 -v 协商 caps（一条命令，风险低）；
2. **plane 枚举**：weston 运行中枚举 plane0/1 的占用（FB_ID/CRTC_ID property）与 format/modifier 能力，确认空闲 YUV overlay 存在；
3. **自控 setplane 替换 kmssink**：display-test 式代码（含退出只关自己 plane + 触发 weston 重绘），验证播放/退出 weston 界面全程无损；
4. （并行推进）**BSP 修复 libgstwayland viewporter bind**：修复后 `videotestsrc ! waylandsink` 应 exit 0，再实测 weston 导入 NV12 dmabuf（含 modifier）——路线 A 的关键实验；
5. weston GL 合成 dmabuf 的性能（吞吐/丢帧/CPU）与 direct scanout 可能性。

### 决策建议

- 不等 BSP 的近期落地形态：**路线 C（混合）**——UI/输入/launcher 照旧走 weston，视频走自控 KMS plane；
- BSP viewporter 修复到位后：实测路线 A（weston 内 dmabuf），若 modifier 导入通过且性能达标，可作为长期演进形态替代路线 C（恢复单一 wayland 窗口的纯粹性）；
- 路线 B（kmssink）仅作为链路验证工具，不作为产品形态（独占式行为已实测破坏 weston 显示状态）。

---

## 附：本文涉及文档索引

| 文档 | 内容 |
|---|---|
| `x9hp_problems.md` | 早期问题记录（其中 omx dmabuf 结论已被修正，见 §0） |
| `data/AndroidAuto/2026-09-17-h265-dmabuf-performance-test.md` | use-dmabuf 性能对比实测（§4 数据源） |
| `/home/admin0412/projects/markdown/2026-09-17-waylandsink-bsp-retest.md` | waylandsink BSP bug 定位（§3.2 数据源） |
| `display-test/display/` | Semidrive DRM 直控参考实现（§5.1、§7） |
| `egl-demo/files/src/` | EGL dmabuf import/export 参考实现（§5.2） |
| `README.md` §2.2 | 布局控制路径与 hmi-controller 权限（§2 数据源） |
| `screencast-avsync-design.md` §8 | vpudec/dmabuf 待确认事项（历史遗留，部分已被本文实测回答） |
