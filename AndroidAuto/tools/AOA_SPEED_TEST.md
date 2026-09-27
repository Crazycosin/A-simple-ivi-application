# AOA USB 传输速率测试

测试 `31260000.dwc3` 与 Android 手机的 AOA Bulk 链路。板端工具独立于
AndroidAuto 应用、aasdk/openauto，不修改第三方代码。手机端是自有 Java 测试
APK，使用 Android 系统 USB accessory API，无谷歌 SDK、网络或存储依赖。

## 构建

在本目录交叉编译板端工具：

```bash
make aoa_speed_test
```

Makefile 自动加载现有 X9 SDK 环境，产物为 AArch64 动态链接程序，板端需有
`libusb-1.0.so.0`。原有 `make aoa_probe` 保留。

手机 APK 需要本机已有 JDK（推荐 17）、Android SDK 平台 35 和 Build Tools
35.0.0。构建脚本不自动安装或下载依赖，也不修改 SDK 文件。

```bash
export JAVA_HOME=/path/to/jdk
export ANDROID_SDK_ROOT=/path/to/android-sdk
make apk
```

可通过 `ANDROID_PLATFORM`、`ANDROID_BUILD_TOOLS` 选择已有版本；编译平台需
至少 API 33，默认 target SDK 35。APK 输出至
`aoa_speed_android/out/aoa-speed-test.apk`，使用该目录内自生成的测试签名。
APK 最低 Android 6.0，测试签名仅用于本地测试。
本环境没有 JDK/完整 Android SDK，构建依赖下载未获批准，因此当前交付源码和
构建脚本，未生成或实际编译验证 APK。

先通过主机直连手机安装，确认选择的是手机，不是板子的 ADB 连接：

```bash
adb -s PHONE_SERIAL install -r aoa_speed_android/out/aoa-speed-test.apk
```

## 板端运行

1. 退出 AndroidAuto 应用及其他占用 AOA 的程序。
2. 将工具复制到板端，赋予执行权限；保留之前为该控制器配置的 udev 权限规则。
3. 重新插拔手机，使用支持数据传输的线缆连接到 `31260000.dwc3` 对应接口。
   若刚运行过 `aoa_probe`，务必重插：其 AOA 身份与本 APK 不同。
4. 运行工具，手机进入 AOA 测试模式后选择 **AOA Speed Test** 并授权。
   如未自动打开，手动启动 APK，点击“连接 / 授权”。保持 APK 在前台。

```bash
./aoa_speed_test
```

默认流程：每个方向 2 秒完整校验；每个方向 3 轮，每轮独立预热 3 秒、正式
测速 30 秒。两个方向顺序执行，数据只在内存中处理，默认 32 个 16 KiB 异步
请求在途，总缓冲约 512 KiB。默认测速约 202 秒，另加授权与模式切换时间。

工具先显示候选设备端口路径；多部手机时明确选择：

```bash
./aoa_speed_test --port 1-1.2
./aoa_speed_test --seconds 10 --warmup 2 --rounds 1 --queue 32
./aoa_speed_test --help
```

`--port` 是总线与物理端口路径，不是设备 address；同一次启动中用于找回
AOA 重新枚举后的手机。默认只自动选择 AOA/ADB/成像类候选；无法自动识别的
手机可明确指定端口。不要指定未知外设端口。重启后 bus number 可能改变。
工具不会解绑内核驱动，不使用 ADB 接口的数据端点；接口占用时报错退出。

## 统计与通过条件

工具显示实际协商的 USB 速度。`aoa_version=2` 不表示 USB 2/USB 3 速度等级。
High-Speed 原始线速 480 Mb/s，即 60 MB/s，实际载荷还受协议开销、手机、内核、
控制器和用户态处理影响。50 MB/s 是验收目标，不能由 AOA 版本推断或保证。

```text
RESULT phone->board round=1 receiver=xx.xx MB/s bytes=... duration=... frames=... errors=... PASS/BELOW_TARGET
RESULT board->phone round=1 receiver=xx.xx MB/s ...
PRIMARY phone->board: PASS/FAIL (each of 3 rounds >= 50.00 MB/s).
```

- MB = 1,000,000 字节；50 MB/s = 400 Mb/s，不是 50 MiB/s。
- 有效载荷不包含每帧 32 字节测试头、USB 协议开销或 ADB 流量。
- 手机→板端：板端实际解析的载荷 / 板端接收耗时；必须与手机发送统计一致。
- 板端→手机：以手机实际读取的载荷和手机本地单调时钟统计为准，板端完成
  的 OUT 字节只用于核对。每秒 `USB sender` 行不是最终接收吞吐结论。
- 耗时从该方向开始处理数据到最后一帧完整数据，包含首次数据等待时间；
  不包含手动授权、AOA 切换和预热。两个方向不相加，不使用往返速度代替单向。
- 主要通过条件：手机→板端每轮正式测速均 >= 默认 50 MB/s，双方数量一致、
  无传输/序号错误，两个方向完整校验通过。反向阈值结果单独报告。
- 完整 payload 校验仅在独立校验阶段执行；正式测速仍验证帧头、run、序号和
  双方计数。`errors=0` 不代表正式测速逐字节校验过每个 payload。
- `--min-mb-s` 的数值单位为 MB/s；
  默认 50。推荐使用默认值验收。
- 返回码：0=主要指标通过；2=测速完成但主要指标不达标；1=错误或中断，
  不应视为有效验收结果。

## 协议和资源释放

小端 32 字节帧头：magic `0x53504f41`、version 1、type、payload length、run、
64 位 sequence 和 8 字节保留字段（全零）。每帧最多 16 KiB，数据 payload
16352 字节。双方按字节流解析，不假定一次 USB 传输恰好对应一帧。
手机始终用完整 16 KiB 原始缓冲接收，防止小缓冲截断 accessory 数据。

先由 APK 发送 HELLO `AOASPEED`，才允许发送自定义测速数据；每个阶段
COMMAND → READY → START → DATA 流。手机发送方向以 REPORT 结束；板端发送
方向先完成所有 OUT 请求，再发 END，由手机回 REPORT。结束全部测试后发
FINISH，手机关闭测试连接。这个协议验证的是通用 AOA 链路，不是 AA 协议握手。

板端每个传输超时 5 秒，等待手机首次授权/HELLO 最多 90 秒。Ctrl+C 中断时
取消并收完异步回调，再释放接口和缓冲。手机“停止”关闭描述符；若设备内核
仍阻塞 I/O 或要切换回原 USB 模式，重新插拔手机。只释放接口，不主动重置设备。

## 自动验证与板端待验

```bash
make test
# 有 JDK 时，手机协议/接收端逻辑还可直接在 JVM 测试，不需要 Android SDK：
bash aoa_speed_android/test.sh
```

C++ 测试覆盖小端计数与单位、分片/合并流、坏帧头；模拟 USB 对端覆盖异步
回调乱序、短读、接收统计核对、载荷损坏、序号错误、超时及取消在途请求。
这些测试不访问真实设备，不能证明 >=50 MB/s。

板端仍需验证 APK 构建安装、AOA 模式切换及三轮真实速率；同时记录手机型号、
线缆、USB 协商速度及队列深度。若不达标，可比较 `--queue 8/16/32/64`，观察
是否受请求流水线影响；U盘或 ADB 测速结果不能代替本测试的手机 AOA 结果。
