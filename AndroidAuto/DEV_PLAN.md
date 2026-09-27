# AndroidAuto 应用开发计划

> 版本：v2.0（2026-09-16，按现代软件工程规范重构）
> 位置：`data/AndroidAuto/`（git 仓库根）
> 交叉编译环境：`. /home/admin0412/x9sp_wayland/environment-setup-cortexa55-sdrv-linux`

---

## 1. 项目目标

基于设备现有 ivi-shell 环境（x9m_ms PTG6.1 / Weston 13 + 定制 hmi-controller），开发名为 **AndroidAuto** 的可点击应用：

| 需求 | 说明 |
|---|---|
| launcher 入口 | 现有 launcher 上显示带 icon 的入口（`[ivi-launcher]` 配置，icon-id=4007） |
| 点击启动 | 点击 icon 启动 app，surface（ivi_id=3010）挂入 application layer |
| 右上角关闭 | 应用内右上角关闭按钮，**up 事件**命中热区才退出（防误触） |
| 播放窗口占位 | 内容区 16:9 带边框圆角占位窗口，本期不播放内容 |

**不在本期范围**：视频播放接入（预留 `Layout::videoWindow()` 矩形接口）、Weston 16 源码编译（可选任务，已确认不做）。

---

## 2. 已验证的开发基础

| 项 | 状态 |
|---|---|
| 交叉环境 wayland-client 1.22.0 + cairo 1.18.0 + 原生 wayland-scanner | ✅ 依赖检查 + 冒烟编译均通过 |
| 设备侧 launcher / 触控 / 归层机制（`start_ivi.sh` 方案） | ✅ 已验证 |
| 协议 XML（`protocol/ivi-application.xml`，自 weston 源码复制） | ✅ |
| 运行时库与设备 Weston 13 同源（ABI 匹配，无需打包 .so） | ✅ |

## 3. 架构设计（MVC）

```
┌─────────────────────────── AndroidAuto 进程 ───────────────────────────┐
│                        Controller（src/app/）                          │
│   InputController：原始输入事件 → 语义动作（关闭/埋点）                 │
│   AndroidAutoApp：组合根，装配 M/V/基础设施，运行主循环               │
└──────────┬───────────────────────────────────────────┬────────────────┘
           │ 读状态/写状态                               │ paint()
┌──────────▼───────────────────┐       ┌───────────────▼────────────────┐
│ Model（src/model/）           │       │ View（src/view/）               │
│  AppConfig：ini 配置解析      │◄──────│  AppView：cairo 绘制            │
│  AppModel ：运行时状态/尺寸   │ 只读  │  Layout：纯几何计算（无状态）    │
└──────────▲───────────────────┘       └───────────────▲────────────────┘
           │                                              │
┌──────────┴──────────────────────────────────────────────┴────────────┐
│ 基础设施（src/wayland/）+ 工具（src/util/）                             │
│  WlClient ：连接/registry/ivi surface/seat，C 回调 thunk→std::function │
│  ShmBuffer：RAII shm 缓冲 + cairo surface（move-only）                │
│  Log      ：RAII 文件日志                                               │
└────────────────────────────────────────────────────────────────────────┘
```

**事件流**：wayland 事件 → `WlClient` thunk → Controller（`InputController`）→
更新 `AppModel`（置 dirty）→ 主循环检测 dirty → `AppView::paint()` → commit。

**surface id / layer id 分配**（已排查冲突）：app surface **3010**（application 段
3000–3999），launcher icon **4007**（已用到 4006）。app 自声明 id，不依赖定制
launcher 的 `surface-id=` 机制。

---

## 4. 工程规范（强制）

| 规范 | 落实方式 |
|---|---|
| MVC 分层 | `src/{app,model,view}/` 目录隔离；View 只读 Model；Controller 唯一可写 |
| h/cpp 分离 | 所有类声明在 `.h`，实现在 `.cpp`；`#pragma once` |
| 定宽类型 | `<cstdint>`；坐标 `std::int32_t`（wayland 协议宽度），id/尺寸小值 `std::uint16_t`，surface_id `std::uint32_t`；接口不用裸 `int` |
| RAII | wayland/cairo/memfd/mmap 资源全部由类析构释放；`ShmBuffer`/`Log` move-only，转移所有权 |
| 智能指针 | `std::unique_ptr<AppView>` 延迟构造；禁裸 new/delete |
| 禁止硬编码 UI/路径 | 全部 UI 尺寸、颜色、文案、路径来自 `config/androidauto.ini`；相对路径基于 ini 所在目录解析 |
| 资源目录 | `res/icons/`（PNG 图标/标题 logo）、`res/shaders/`（预留 GL 资源）；由 ini 引用，代码不出现路径字面量（`/proc/self/exe` 定位除外） |
| 构建 | Makefile：`all / clean / protocol`；依赖自动生成（-MMD）；产物 `bin/` `obj/` 被 .gitignore 过滤 |
| 版本管理 | git；`make clean` 后仓库只含源码、配置、文档、资源 |

## 5. 目录结构

```
AndroidAuto/
├── DEV_PLAN.md / TEST_PLAN.md / README.md
├── .gitignore
├── Makefile
├── config/
│   └── androidauto.ini          # 运行时配置（UI 度量/配色/路径）
├── protocol/
│   └── ivi-application.xml      # 协议定义（生成 glue 到 obj/，不入库）
├── res/
│   ├── icons/androidauto.png    # launcher 图标 + 标题 logo
│   └── shaders/                 # 预留
├── src/
│   ├── main.cpp                 # 入口：定位 exe 目录 → AppConfig → App
│   ├── app/                     # Controller
│   │   ├── AndroidAutoApp.h/.cpp
│   │   └── InputController.h/.cpp
│   ├── model/                   # Model
│   │   ├── AppConfig.h/.cpp     # ini 解析 + 默认值
│   │   └── AppModel.h/.cpp      # 窗口尺寸/运行状态/dirty
│   ├── view/                    # View
│   │   ├── Layout.h/.cpp        # 纯几何
│   │   └── AppView.h/.cpp       # cairo 绘制
│   ├── wayland/                 # 基础设施
│   │   ├── WlClient.h/.cpp
│   │   └── ShmBuffer.h/.cpp
│   └── util/
│       └── Log.h/.cpp
├── bin/  （产物，gitignored）
└── obj/  （产物+协议 glue，gitignored）
```

## 6. 关键技术决策

1. **双缓冲 + release 监听**：`ShmBuffer` 挂 `wl_buffer` release 事件置非 busy；
   repaint 时 `acquireFreeBuffer()`，两块均忙则 `dispatch()` 等待释放——消除撕裂。
2. **首帧用默认尺寸**：定制 hmi-controller 需 surface 有内容（commit 过 buffer）才归层，
   之后 configure 事件给实际尺寸 → `AppModel::setWindowSize` → dirty → 重分配重绘。
3. **输入坐标**：pointer 事件本身不带坐标（button 只有序列号），由 enter/motion 跟踪；
   touch up 事件不带坐标，按 touch id 记录最后位置。
4. **退出 = 进程退出**：无需协议请求，surface 销毁后 controller 自动重排回 launcher。

## 7. 构建与部署

```bash
# 1. 生成协议 glue（Makefile 自动执行，也可手动）
make protocol
# 2. 编译（需先 source SDK 环境，见 README）
make
# 3. 部署：adb push data/AndroidAuto /data/AndroidAuto
#    weston.ini 已追加 launcher 条目 → adb shell /data/start_ivi.sh
```

## 8. 里程碑

| 里程碑 | 内容 | 验收 | 工期 |
|---|---|---|---|
| M1 | 工程骨架 + wayland/ivi 连接 + 默认尺寸首帧 | journalctl 无错，surface 出现在 application layer | 0.5 天 |
| M2 | cairo UI（标题栏/logo/边框窗口占位） | 上板目检布局正确 | 0.5 天 |
| M3 | 输入：触控+指针、右上角关闭、播放区坐标日志 | TEST_PLAN TC-C1~C9 | 0.5 天 |
| M4 | launcher 集成、icon、部署联调 | 全部验收用例通过 | 0.5 天 |

## 9. 风险与对策

| 风险 | 对策 |
|---|---|
| 定制 hmi-controller 归层行为与源码有出入 | M1 先验证；兜底走其 `surface-id=` 机制 |
| application layer 尺寸 = 屏高 − panel 高，坐标系不同 | 只用 configure 给的 w/h 建 buffer，布局按本地坐标 |
| 触摸 IC 报点与显示分辨率不 1:1 | TC-C9 采集触点日志标定；必要时 `[libinput]` 校准 |
| windows_share 文件名/权限 | 全 ASCII 文件名；README 部署步骤含 chmod +x |

## 10. 后续演进（本期不实现）

1. 播放接入：`Layout::videoWindow()` 矩形替换为 gst waylandsink（参考 `gst_nv12_wayland` + `720.h264`）；
2. 触控轨迹可视化（touch-trace overlay）；
3. AA 业务逻辑插入主循环 dispatch 空闲期。
