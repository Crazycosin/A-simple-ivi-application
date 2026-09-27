#!/bin/sh
# AndroidAuto 启动包装脚本（weston.ini [ivi-launcher] 的 path 指到这里）。
#
# 作用：source x9hp 自带前缀环境（/opt/x9hp/env.sh），把 /opt/x9hp/lib 加进
# LD_LIBRARY_PATH、/opt/x9hp/lib/gstreamer-1.0 加进 GST_PLUGIN_PATH——那里有
# libgstlibav（avdec_aac AAC 软解）及其 ffmpeg 依赖，rootfs 里没有。
#
# 为什么用包装脚本而不是代码里 setenv：LD_LIBRARY_PATH 是动态加载器在进程
# 启动那一刻读取的，运行中 setenv 对后续 dlopen 的依赖解析不生效；必须在
# exec 二进制之前放进环境。
#
# 兼容性：/opt/x9hp/lib 与应用自身依赖（glib/gstreamer/wayland/cairo）无
# 同名库，LD_LIBRARY_PATH 前置不会劫持 rootfs 版本（已核对）。
#
# HOME=/tmp：launcher 启动时进程身份是 weston 用户（HOME=/home/weston，
# 只读且无 .omxregister）。bellagio OMX core 按 $HOME 找组件注册表，
# 找不到则初始化直接失败（Failed to initialize core 0x80001000，实测
# 连 /dev/vpucoda 都不会尝试）。HOME 指向 /tmp 时 fallback 到
# /tmp/.omxregister（该文件在系统侧已注册），omx 硬解即可正常加载。
export HOME=/tmp

DIR=$(cd "$(dirname "$0")" && pwd)

if [ -f /opt/x9hp/env.sh ]; then
    . /opt/x9hp/env.sh
fi

exec "$DIR/bin/AndroidAuto"
