# USB 能力确认

```
USB 平台能力确认（Host 规格 / AOA V1.0 / CDP / 供电）；

Host规格

AOA V1.0: 板子上有usb3.0,2.0高速兼容口。（5000M/s,480M/s）
CDP: SOC不支持CDP，BC1.2，需要加其他芯片。
供电:支持，似乎慢充
热插拔检测：libusb1.0支持

USB 热插拔事件联调；
libusb 数据传输通路打通；
传输速率测试（≥50MB/s）;
```