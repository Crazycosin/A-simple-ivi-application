# 开发计划2
```
1.阅读2026-09-16-CHECKPOINT.md梳理分析目前进度
2.阅读/home/admin0412/projects/ivi_app/ivi_video_player相关代码
3.根据目前代码结构，遵顼目前的代码开发规范，在src/app新增一个VideoPlayer.h/cpp,AudioPlayer.h/cpp,因为之后可能音视频输入是两条路。
4.目前应用打开没有占领除了lanch栏上方所有区域，需要作修改，播放区在该应用窗口下上下左右各隔离20个像素左右，也是可调参数。不可硬编码。
5.目前先按照2里的参考代码实现weston+wayland播放窗口,播放。
6.目前固定播放/data/gstreamer_test/resource/h265_acc_1920那个视频。在config里配置
7.开发音视频播放接口，设计通用性接口，buffer之类的，参考2中的时延统计链路，跟目前log结合。
```