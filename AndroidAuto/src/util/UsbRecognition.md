# USB 最新状态查询

`AndroidAutoApp::mainLoop()` 通过 `StartUsbMonitoring()` 和
`StopUsbMonitoring()` 管理监控。后台使用 libusb 热插拔通知识别
`31260000.dwc3` 下的设备，启动时识别已连接设备，不周期枚举。

主线程或其他线程直接查询：

```cpp
const auto status = aa::UsbDeviceMonitor::Instance().GetStatus();
if (status.state == aa::UsbStatus::State::kMonitoring) {
  if (status.devices.empty()) {
    // 当前没有已识别的连接设备。
  }
  for (const auto& entry : status.devices) {
    // entry.device: bus/address、VID/PID、厂商/产品、接口、AOA 版本、查询错误。
    // entry.recognition: is_phone、aa_candidate、reason。
  }
}
```

`GetStatus()` 仅加锁复制最新状态，不做 USB 查询，不消费事件。
返回副本可安全用于业务策略；设备可经过 Hub 接入，因此返回设备列表。
`state` 区分 stopped、starting、monitoring、error，`message` 给出监控诊断。
monitoring 且列表为空才表示没有已识别设备；starting 表示启动识别尚未完成，
error 时列表仅是最近缓存，应结合错误决定策略。停止后列表清空。
不保留历史通知，不提供发布订阅或主线程通知 fd。

最新状态指后台已处理的最新结果；USB 查询期间或热插拔通知处理前会有短暂延迟。
业务层按需调用，不需要创建周期查询任务。生命周期由拥有者线程管理，
全局仅一份函数内静态单例，由应用主线程启动和停止，读取线程在应用退出前结束。
内部只有一把互斥锁保护状态和 libusb 上下文/回调暂存；停止标志使用 atomic，
不再反复加锁读取。USB 查询不持锁，GetStatus 只短暂持锁复制字符串和列表。
单例统一访问入口，并不能消除后台写入与外围查询的并发。

通过 `/sys/bus/usb/devices/usb<bus>` 的真实路径完整节点名匹配控制器，
包含 USB2/USB3 总线及 Hub 下游设备，跳过根 Hub。拔出使用已缓存信息，
不会依赖消失的 sysfs；重复到达通知去重。回调只入队保留设备引用，
同步识别在回调外执行。后台阻塞等待事件（上限 24 小时），失败设备每 5 秒
重试，停止通过 `libusb_interrupt_event_handler()` 唤醒。
不支持热插拔或注册失败时返回 error，不回退周期轮询。

识别沿用 `tools/UsbDevices.java` 的纯外设排除、AOA VID/PID、MTP/PTP/ADB、
AOA GET_PROTOCOL、复合存储及名称启发式规则。沿用 AOA >= 1 和华为 VID
的 AA 候选排除；Apple VID 直接判定仍未启用。关键词可能误判，AOA 响应
不能确认 Android Auto 可用。aoa_version=-1 表示未查询或失败，0 表示不支持。
只查询 AOA 请求 51，不切换设备模式、不 claim 接口、不解绑驱动。

## 验证

```bash
source /home/admin0412/x9sp_wayland/environment-setup-cortexa55-sdrv-linux
make -j2 check
```

主机分类测试：

```bash
g++ -std=c++17 -Wall -Wextra -Wpedantic -Isrc \
  tests/UsbDeviceRecognizerTest.cpp src/util/UsbDeviceRecognizer.cpp \
  -o /tmp/usb_recognizer_test
/tmp/usb_recognizer_test
```

Linux 主机最新状态查询及退出测试（需要 libusb 开发头文件和运行库）：

```bash
g++ -std=c++17 -Wall -Wextra -pthread -Isrc \
  tests/UsbDeviceMonitorTest.cpp src/util/UsbDeviceMonitor.cpp \
  src/util/UsbDeviceRecognizer.cpp $(pkg-config --cflags --libs libusb-1.0) \
  -o /tmp/usb_monitor_test
timeout 15s /tmp/usb_monitor_test
```

测试使用不存在的控制器名，不向主机设备发送请求，验证状态查询、
副本隔离、跨线程读取、启动/停止及退出唤醒，不替代板端真实插拔验证。
板端通过 `readlink -f /sys/bus/usb/devices/usb*` 核实控制器路径，并验证
目标接口、其他控制器、Hub、手机/U盘/HID，以及无权限后恢复权限的重试。
目标系统需已有相容的 `libusb-1.0.so.0` 运行库。
