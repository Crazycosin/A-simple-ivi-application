# AndroidAuto 应用测试计划

> 版本：v1.0（2026-09-15）
> 配套文档：`DEV_PLAN.md`
> 测试对象：`data/AndroidAuto/bin/AndroidAuto`（aarch64）

---

## 1. 测试环境与前置

### 1.1 构建机（host）
- 交叉环境：`. /home/admin0412/x9sp_wayland/environment-setup-cortexa55-sdrv-linux`
- 工具：`file`、`aarch64-sdrv-linux-readelf`、`make`

### 1.2 目标设备（x9m_ms PTG6.1）
- Weston 13 ivi-shell 已按 `data/start_ivi.sh` 启动（bind-mount weston.ini / hmi-controller.so / weston-ivi-shell-user-interface）
- 验证前置：`wayland-info | grep ivi_application` 有输出
- 触控屏 + 鼠标（如有）可用

### 1.3 测试数据
- `data/AndroidAuto/icon/androidauto.png`（128×128）
- 设备上 weston.ini 已含 AndroidAuto 的 `[ivi-launcher]` 条目（icon-id=4007）

## 2. 测试阶段

| 阶段 | 环境 | 内容 |
|---|---|---|
| A 构建验证 | host | 编译产物正确性 |
| B 上板冒烟 | device | 启动链路、归层 |
| C 功能用例 | device | 交互与渲染 |
| D 稳定/性能 | device | 反复启停、资源占用 |
| E 回归 | device | 改动后重跑 P0 用例 |

## 3. 用例表

优先级：P0 = 验收必须，P1 = 重要，P2 = 次要。

### 阶段 A：构建验证（host）

| 编号 | 步骤 | 预期 | 级别 |
|---|---|---|---|
| A-1 | source 环境后 `make clean && make` | 编译 0 error（warning 记录） | P0 |
| A-2 | `file bin/AndroidAuto` | ELF 64-bit ARM aarch64, dynamically linked | P0 |
| A-3 | `aarch64-sdrv-linux-readelf -d bin/AndroidAuto \| grep NEEDED` | NEEDED 仅含：libwayland-client.so.0、libcairo.so.2、libstdc++.so.6、libgcc_s.so.1、libc.so.6（+libm 若用） | P0 |
| A-4 | 无任何权限/文件名异常 | bin/AndroidAuto 在共享盘可见 | P2 |

### 阶段 B：上板冒烟（device）

| 编号 | 前置 | 步骤 | 预期 | 级别 |
|---|---|---|---|---|
| B-1 | start_ivi.sh 已跑 | `wayland-info \| grep ivi_application` | 输出 ivi_application 全局接口 | P0 |
| B-2 | B-1 | launcher 界面出现 AndroidAuto icon | icon 显示、位置正常、无破图 | P0 |
| B-3 | B-2 | 点击 AndroidAuto icon | 进程启动（`pgrep AndroidAuto`）、1–2s 内画面出现 | P0 |
| B-4 | B-3 | `journalctl -u weston --since -1min` | 无 weston 报错/协议错误 | P0 |
| B-5 | B-3 | 目检 app 位置与尺寸 | 全屏位于 application layer 区域（屏高−panel 高），无偏移拉伸 | P0 |

### 阶段 C：功能用例（device）

| 编号 | 步骤 | 预期 | 级别 |
|---|---|---|---|
| C-1 | 启动后目检 | 标题栏深色，右上角显示 ✕ 按钮 | P0 |
| C-2 | 目检播放窗口 | 带边框矩形居中显示于内容区（video_margin 边距、圆角、边框） | P0 |
| C-3 | 触摸点击右上角 ✕ 热区（up 时触发） | app 退出、surface 消失、launcher 重现、布局重排正常 | P0 |
| C-4 | 鼠标（如有）点击 ✕ 热区 | 同 C-3 | P1 |
| C-5 | 触摸点击 ✕ 热区**外**的标题栏空白区 | 不退出（防误触验证） | P0 |
| C-6 | 触摸点击播放窗口内部 | app 不退出；日志输出触点坐标（格式：`[touch] x=… y=…`） | P1 |
| C-7 | 按住 ✕ 不抬手，滑出热区再抬手 | 不退出（up 位置命中才触发） | P1 |
| C-8 | 点击 icon 重复启动（前一实例在跑） | 新实例正常启动或旧实例被脚本清掉（按部署脚本设计） | P1 |
| C-9 | C-6 采集触点坐标与实际显示位置对比 | 坐标映射正确（1:1）；若偏移，记录偏移量为 [libinput] 校准依据 | P0 |

### 阶段 C2：播放功能用例（device，2026-09-16 v1.1 新增）

| 编号 | 前置 | 步骤 | 预期 | 级别 |
|---|---|---|---|---|
| C2-1 | ini [player] enabled=1 | 启动 app，看日志 | `[vplayer]/[aplayer] pipeline starting` + `prerolled, -> PLAYING`，无 error | P0 |
| C2-2 | C2-1 | 目检播放窗口 | 视频画面显示在 videoWindow 矩形内（BGRx 软拷贝路径），宽高比跟随视频（1920x960=2:1） | P0 |
| C2-3 | C2-1 | `cat /proc/asound/card1/pcm0p/sub0/hw_params` | 打开且 S16_LE / 2ch / 48000（板级唯一验证格式） | P0 |
| C2-4 | C2-1 | 看日志 `[startup]` 行 | 首帧上屏 < 1s（实测 ~500ms） | P1 |
| C2-5 | C2-1 | 看日志 `[latency]` 行（每 latency_interval 帧一行） | read→pull / pull→commit / commit→screen / e2e 四段齐全，数字合理（实测 7/10ms + e2e ~1s 含 1s 同步缓冲） | P1 |
| C2-6 | C2-1 | 看日志 `[drift]` 行（每 drift_interval_sec 一行） | 两管线 position 差值打印（不同源时 diff 大属正常，仅监测） | P2 |
| C2-7 | 视频 EOS（loop=1） | 等视频播完一轮 | 自动 seek 回 0 循环，无 error 日志 | P0 |
| C2-8 | `kill -TERM <pid>` | 发 SIGTERM 后看日志 | `[app] signal received` → 停两管线 → `[latency-summary]` 总结 → 退出，无残留进程 | P0 |
| C2-9 | ini video_uri 改不存在路径 | 启动 app | vplayer 报 error 但应用不退出（占位窗口正常显示） | P1 |
| C2-10 | ini fullscreen_layout=1（需 compositor 侧放行，见 README「布局控制路径」） | 启动/退出 | 启动全屏、退出恢复 tiling | P1（当前阻塞） |

### 阶段 D：稳定性 / 性能（device）

| 编号 | 步骤 | 预期 | 级别 |
|---|---|---|---|
| D-1 | 启动（命令行直启）→ SIGTERM 退出，循环 10 次 | 每次均正常退出（含时延总结），无崩溃/残留进程 | P0 |
| D-2 | 音视频同跑静置 5 分钟，每 30s 采 VmRSS/VmData | VmData 在 ~5 分钟内封顶（双管线高负载下 GStreamer buffer pool 弹性水位 ~+16MB，非泄漏；实测 205MB 稳定） | P1 |
| D-3 | 播放中 CPU 占用 | 60fps NV12→BGRx 软转换 + cairo 缩放预期高占用（实测 ~176%）；空闲（enabled=0）时应 ≈0% | P1 |
| D-4 | 与 weston-simple-egl（3002）同开 | 分层正确，互不遮挡错乱 | P1 |
| D-5 | app 运行中重启 weston（systemctl restart weston） | weston 恢复后 launcher 正常；app 退出无僵死（允许被杀） | P2 |

### 阶段 E：回归范围

任何代码改动后至少重跑：A-1~A-3、B-3、C-2、C-3、C-5、C-9、D-1。

## 4. 测试取证手段

| 手段 | 用途 |
|---|---|
| `journalctl -u weston -f` | weston 侧日志、surface 归层行为 |
| `AndroidAuto 2>/tmp/aa.log`（部署脚本重定向） | app 侧触点坐标日志 |
| `WAYLAND_DEBUG=1 AndroidAuto` | wayland 协议全量跟踪（排障用，正常用例关闭） |
| `grep VmRSS /proc/$(pidof AndroidAuto)/status` | 内存 |
| `top -p $(pidof AndroidAuto)` | CPU |
| 相机拍摄屏幕（ivi-shell 无截图绑定时的目检取证） | UI 布局记录 |

## 5. 验收标准

1. 阶段 A、B 全部 P0 通过
2. 阶段 C 全部 P0 通过（含 C-9 坐标映射或已给出校准方案）
3. 阶段 D：D-1 通过；D-2/D-3 无异常结论
4. 无未关闭的 P0/P1 缺陷

## 6. 缺陷记录格式

```
[DEF-编号] 标题
  现象：实际表现（含日志/照片）
  复现：用例编号 + 步骤
  环境：构建时间 / weston 日志片段
  状态：open / fixed / verified
```
