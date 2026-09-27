# 开发计划3
```
1.阅读2026-09-17-CHECKPOINT.md跟 2026-09-17-media-pipeline-appsrc-live-deadlock.md
接下来回答我以下几个问题，不要修改任何文件。
a.目前video硬件解码发生在哪个阶段,输出的buffer是什么格式yuv还是bgr。之后会发生几次拷贝到上屏。
"""

"""


b.目前应用通过lancher点击启动后固定在左上角位置，大小也固定，可以调整吗，可以怎么调整，哪些可以调整哪些不可以。
c.之前的排查信息中，提到播放窗口无法设置到只留底部lancher栏，其他全屏的原因是什么，要做什么修改去了，可行路径是怎么样的。

d.如果要实现dmabuffer实现到上屏的零拷贝路线，目前这个应用方案（weston+wayland）能否行得通,不行的原因是什么，需要哪些调整？
e.如果使用kms方案在本应用里能否行得通，行不通的原因是什么？

f.display-test,跟egl-demo的渲染上屏方案是什么，能否在weston基础上参考，不能的话，不考虑weston可否设计一个带dmabuf的视频渲染方案。

```

## 
```
是的，DRM 直扫这条路径就是全程 NV12，没有转 RGB 这一步，这是刻意设计成这样的，不是漏掉了。

硬件解码在哪：不是编译期声明，是运行时按编码格式动态创建的 GStreamer element，在 MediaPlayer.cpp:205-213：


if (fmt == "h265") { decName = "omxh265dec"; }
else if (fmt == "h264") { decName = "omxh264dec"; }
dec = gst_element_factory_make(decName.c_str(), "v_dec");
omxh265dec/omxh264dec 是 vendor OMX 插件包提供的 GStreamer 元素名，底层挂的是 Semidrive WAVE VPU 硬件解码核心——这个 app 没有自己写解码逻辑，只是按名字实例化这个元素；如果 factory 创建失败（返回 null），MediaPlayer.cpp:214-219才会回退到 avdec_h264/avdec_h265 软解。DRM 路径下还会在这个 dec 上开 use-dmabuf=true（MediaPlayer.cpp:224-229），触发零拷贝 dmabuf 输出。

为什么没转 RGB：MediaPlayer.cpp:231-233的注释写得很直白——"不插 videoconvert/capsfilter：任何内存格式转换都会把 dmabuf 拉回系统内存（拷贝路径）；NV12 直扫由 DRM plane 完成"。链路就是 appsrc → queue → parse → dec → appsink，中间完全没有色彩空间转换环节。

NV12 能直接上屏靠的是硬件：DrmView.cpp:35默认走 DRM_FORMAT_NV12，MediaPlayer.cpp:64-69把 GStreamer 的 NV12/NV21/NV16/NV24 映射成对应的 DRM fourcc（BGRA/BGRx→ARGB8888/XRGB8888 那两支是 shm 软件绘制路径专用的，dmabuf 硬解路径走的一直是 YUV 那几支）。这块 DPU 的 overlay plane 本身就支持 NV12 作为原生输入格式（之前用 modetest -p 查过 plane 的 IN_FORMATS 列表确认过），YUV→RGB 的色彩空间转换是显示控制器（DPU）在做 plane 合成/scanout 到面板输出时用硬件 CSC 单元完成的，不需要 CPU/GPU 参与，也就不需要 pipeline 里插转换元件——这正是"直扫"这条路线省掉一次拷贝+一次转换的地方。
```