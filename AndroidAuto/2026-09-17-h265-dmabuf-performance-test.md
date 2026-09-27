# H.265 解码 DMABuf 零拷贝性能对比测试

日期：2026-09-17
板子：SemiDrive X9HP（aarch64），设备 `0123459876`
被测脚本：`/data/gstreamer_test/test_h265.sh`

---

## 1. 结论速览

| 场景 | 不带 dmabuf | `use-dmabuf=true` | 差异 |
|---|---|---|---|
| **完整脚本**（视频 + 音频，端到端） | **22.229 s** | **16.769 s** | 快 5.5 s（−25%） |
| 视频-only 实时播放 | 21.502 s | 16.636 s | 快 4.9 s |
| **纯解码吞吐**（963 帧，`fakesink sync=false`） | **20.735 s ≈ 46 fps** | **3.061 s ≈ 315 fps** | **6.8×** |
| CPU `user` 时间（视频-only 播放） | **20.703 s** | **0.887 s** | **23×** |

**基准线：16.05 s**（视频本身时长，963 帧 @60 fps）。

- 带 dmabuf：16.7 s ≈ 实时，**一帧不丢**。
- 不带 dmabuf：22.2 s，等效 **43.3 fps**，相对 60 fps 源**丢帧约 28%** —— 这就是肉眼看到的"卡"。
- 本质差异：解码输出是否发生 **TILED → LINEAR 逐帧拷贝**。开 dmabuf 后 CPU 几乎退出数据通路。

---

## 2. 背景与被测对象

`test_h265.sh` 内容（关键部分）：

```sh
#!/bin/bash
export XDG_DATA_HOME=/etc/xdg

if [ "$1" = "--use-dmabuf" ]; then
    export GST_DEBUG="3,omxh265dec:6,omxvideodec:6,gstomxvpucomp:6,gstomxvpuapi:6,gstomxvpucore:6"
    export GST_DEBUG_FILE=gst_debug.log
    DEC_OPTS="use-dmabuf=true"
else
    DEC_OPTS=""
fi

gst-launch-1.0 -v \
    filesrc location=/data/gstreamer_test/resource/h265_aac_1920x960_60fps.mp4 ! \
    qtdemux name=demux \
    demux.video_0 ! queue ! h265parse ! omxh265dec $DEC_OPTS ! \
    kmssink driver-name=semidrive connector-id=79 plane-id=43 \
    demux.audio_0 ! queue ! aacparse ! avdec_aac ! audioconvert ! audioresample ! \
    audio/x-raw,format=S16LE,channels=2,rate=48000 ! alsasink
```

被测素材规格（`gst-discoverer-1.0` 实测）：

| 项 | 值 |
|---|---|
| 文件 | `h265_aac_1920x960_60fps.mp4`（1.4 MB） |
| 时长 | **0:00:16.050000000** |
| 分辨率 | 1920 × 960 |
| 帧率 | 60/1 → **963 帧** |
| 音频 | MPEG-4 AAC (LC) |

`use-dmabuf` 是 `omxh265dec` 的属性（`Whether to use dmabuf for buffer zero copy with sink`），**默认 false**。
它决定解码器输出端口是否声明 `video/x-raw(memory:DMABuf)`：不开则输出系统内存 `video/x-raw`，kmssink 只能拷贝后再扫描。

---

## 3. 测试思路：分三层剥离

只测"跑完要多久"是不够的——**实时播放有音视频同步，wall clock 会被时钟拉住**，测不出解码能力的真实上限，也无法区分瓶颈在解码还是在显示。所以分三层：

| 层次 | 命令特征 | 回答的问题 |
|---|---|---|
| **A. 端到端** | 完整脚本（视频 + 音频 + kmssink + alsasink） | 用户观感：卡不卡？实际多少秒？ |
| **B. 视频-only 实时播放** | 去掉音频分支，仍走 kmssink 同步 | 排除音频干扰，看纯视频链路 |
| **C. 纯解码吞吐** | `fakesink sync=false` | 去掉显示同步，测解码器能力上限（fps） |

判据与读法：

1. **基准线 16.05 s**：A/B 的 `real` 若明显大于它，说明解码跟不上 60 fps，必然丢帧。丢帧率 ≈ `1 − 16.05 / real`。
2. **`user` vs `real` 的比值**：`user ≈ real` 表示 CPU 打满（拷贝/转换瓶颈）；`user << real` 表示 CPU 空转等硬件/时钟（零拷贝或 IO 瓶颈）。
3. **C 层是唯一能看出"能力上限"的**：A/B 受 60 fps 实时节奏限制，最快也就是 16.05 s；C 层不限速，差距才暴露出来。

---

## 4. 测试过程与原始数据

> 所有命令都在 `adb shell` 里执行，并先加载 `/etc/profile.d/x9hp.sh`（原因见 §7.1）。
> 计时用 `bash -c "time <pipeline>"`。

### A. 端到端（完整脚本）

```sh
cd /data/gstreamer_test && . /etc/profile.d/x9hp.sh
bash -c "time bash test_h265.sh"                     # 不带 dmabuf
bash -c "time bash test_h265.sh --use-dmabuf"        # 带 dmabuf
```

```
不带 dmabuf :  real 0m22.229s   user 0m21.817s   sys 0m1.214s
带   dmabuf :  real 0m16.769s   user 0m2.792s    sys 0m2.217s   # 含 GST_DEBUG 写日志
```

### B. 视频-only 实时播放

```sh
# 不带 dmabuf
bash -c "time gst-launch-1.0 filesrc location=resource/h265_aac_1920x960_60fps.mp4 ! \
  qtdemux name=d d.video_0 ! queue ! h265parse ! omxh265dec ! \
  kmssink driver-name=semidrive connector-id=79 plane-id=43"

# 带 dmabuf：omxh265dec 后加 use-dmabuf=true
```

```
不带 dmabuf :  real 0m21.502s   user 0m20.703s   sys 0m1.118s
带   dmabuf :  real 0m16.636s   user 0m0.887s    sys 0m0.639s
```

### C. 纯解码吞吐（不限速）

```sh
bash -c "time gst-launch-1.0 filesrc location=resource/h265_aac_1920x960_60fps.mp4 ! \
  qtdemux name=d d.video_0 ! queue ! h265parse ! omxh265dec [use-dmabuf=true] ! \
  fakesink sync=false"
```

```
不带 dmabuf :  real 0m20.735s   user 0m20.701s   sys 0m0.325s   →  963 / 20.735 =  46.4 fps
带   dmabuf :  real 0m3.061s    user 0m0.461s    sys 0m0.343s   →  963 /  3.061 = 314.6 fps
```

### 公平性校核：`GST_DEBUG` 的开销

`--use-dmabuf` 分支会开 6 级日志并写 `gst_debug.log`，怀疑"带日志反而更快"不公平，补跑一次**不带 DEBUG 的同参数 pipeline**（音频 + dmabuf）：

```
带 dmabuf + 音频 + 无 DEBUG :  real 0m16.706s   user 0m1.325s   sys 0m0.983s
```

对比带 DEBUG 的 16.769 s：`real` 几乎不变（+0.06 s），只有 `user` 从 1.3 s 涨到 2.8 s（写日志的开销）。
**结论：DEBUG 不影响对比结论。**

---

## 5. 数据分析

### 5.1 丢帧量化

不带 dmabuf 端到端 22.229 s：

```
实际等效帧率 = 963 / 22.229 = 43.3 fps
丢帧率       = 1 − 16.05 / 22.229 = 27.8%   （约丢 268 帧）
```

且 C 层显示其解码能力上限只有 **46.4 fps < 60 fps**，所以**无论怎么播都必然丢帧**，不是偶发抖动。

### 5.2 瓶颈不在显示侧

C 层把 sink 换成 `fakesink`（完全不显示）后，不带 dmabuf 仍是 20.735 s，与 B 层 21.502 s 基本一致。
**说明耗时不在 kmssink，而在解码器输出侧。**

### 5.3 瓶颈是 CPU 拷贝

- 不带 dmabuf：`user` 20.703 s ≈ `real` 21.502 s → **CPU 100% 忙于拷贝**。
- 带 dmabuf：`user` 0.887 s << `real` 16.636 s → CPU 基本空转，等 VPU + 时钟。

数据量估算（NV12，1920×960）：

```
每帧 = 1920 × 960 × 1.5 ≈ 2.76 MB
963 帧 ≈ 2.66 GB 的 TILED → LINEAR 转换 + 拷贝
```

日志里满屏出现的 `FLUSH LINEAR(-3) TILED(-3)` 正是这个动作的直接证据。

### 5.4 为什么开了 dmabuf 就不卡

`use-dmabuf=true` 后解码器输出端口声明 `video/x-raw(memory:DMABuf)`，kmssink 直接 `drmModeAddFB` 扫描 VPU 产出的 buffer，**零拷贝**：

- CPU `user` 从 20.7 s 降到 0.9 s（**23×**）
- 解码吞吐从 46 fps 升到 315 fps（**6.8×**），远超 60 fps 需求
- 端到端 16.7 s ≈ 视频时长 16.05 s，说明完全跟上实时节奏

---

## 6. 结论

1. **"卡"的根因是解码输出的系统内存拷贝**，不是 VPU 解码能力不足、也不是 kmssink 慢。
2. **必须显式开 `use-dmabuf=true`**；该属性默认 false，不开就走拷贝路径，1080p 级别（1920×960@60）必然掉到 ~43 fps。
3. **判据**：解码吞吐 fps（C 层）低于源帧率 → 必然丢帧；`user ≈ real` → CPU 拷贝瓶颈。
4. 分辨率越高、帧率越高，差距越大；小分辨率（176×144）素材看不出差别，这就是为什么之前 h264 小分辨率测试没暴露问题。

---

## 7. 踩坑与注意事项

### 7.1 `adb shell` 下 `avdec_aac` 报 `no element`

```
WARNING: erroneous pipeline: no element "avdec_aac"
```

**不是插件没装**。`/opt/x9hp/lib/gstreamer-1.0/libgstlibav.so` 在板子上（9/11 就装好了），
但 `adb shell` 是**非登录 shell**，不加载 `/etc/profile.d/x9hp.sh`，`GST_PLUGIN_PATH` 里没有 `/opt/x9hp/lib/gstreamer-1.0`。

解决：跑之前先

```sh
. /etc/profile.d/x9hp.sh        # 或 . /opt/x9hp/env.sh
```

登录 shell（串口/SSH）会自动加载，所以手动在板子上跑脚本时不会遇到。

### 7.2 两个脚本的 connector-id 不同

`test_h264.sh` 用 `connector-id=77`，`test_h265.sh` 用 `connector-id=79`。照抄 pipeline 时注意改，否则 kmssink 起不来。

### 7.3 `GST_DEBUG` 的写入开销

`--use-dmabuf` 分支会写 `gst_debug.log`（6 级日志，一次约几百 KB ~ 数 MB）。
对 `real` 影响可忽略，但对 `user` 有影响（1.3 s → 2.8 s）。
**做性能对比时，两组要么都开、要么都不开。**

### 7.4 计时方式

用 `bash -c "time ..."`。板子默认 shell 是 busybox ash，不支持 `time`。

---

## 8. 复现命令（一次跑完对比）

```sh
adb shell
cd /data/gstreamer_test && . /etc/profile.d/x9hp.sh

V=resource/h265_aac_1920x960_60fps.mp4

# A. 端到端
bash -c "time bash test_h265.sh"
bash -c "time bash test_h265.sh --use-dmabuf"

# B. 视频-only
bash -c "time gst-launch-1.0 filesrc location=$V ! qtdemux name=d d.video_0 ! queue ! h265parse ! \
  omxh265dec ! kmssink driver-name=semidrive connector-id=79 plane-id=43"
bash -c "time gst-launch-1.0 filesrc location=$V ! qtdemux name=d d.video_0 ! queue ! h265parse ! \
  omxh265dec use-dmabuf=true ! kmssink driver-name=semidrive connector-id=79 plane-id=43"

# C. 纯吞吐
bash -c "time gst-launch-1.0 filesrc location=$V ! qtdemux name=d d.video_0 ! queue ! h265parse ! \
  omxh265dec ! fakesink sync=false"
bash -c "time gst-launch-1.0 filesrc location=$V ! qtdemux name=d d.video_0 ! queue ! h265parse ! \
  omxh265dec use-dmabuf=true ! fakesink sync=false"
```

---

## 9. 建议

1. **把 `use-dmabuf=true` 设为默认**，需要对比时再走 `--no-dmabuf`：
   ```sh
   if [ "$1" = "--no-dmabuf" ]; then DEC_OPTS=""; else DEC_OPTS="use-dmabuf=true"; fi
   ```
2. 所有走 `omx*dec ! kmssink` 的脚本（h264/h265/其它分辨率）都应加这个属性，否则高分辨率必然掉帧。
3. 后续若做分辨率/码率对比测试，直接用 §3 的 C 层（fakesink）测吞吐，最快也最能反映解码能力上限。

---

## 附：相关文档

- `markdown/readme.md` —— x9hp_env.sh 环境工具（`/opt/x9hp` 前缀、`pack-only` 最小打包）
- `markdown/x9hpbundle/components.list` —— ffmpeg / gst-libav 构建记录（`avdec_aac` 的来源）
- `vpu_virtio_backend_issue_analysis.md` —— VPU virtio 后端历史问题（与本次性能无关，但涉及 VPU 自检信息为 0 的现象）
