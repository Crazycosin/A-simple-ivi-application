#!/bin/sh
# AndroidAuto 部署脚本：只推送运行时必需文件（不含源码）
#
# 用法:
#   ./push.sh              # 推送运行时文件并重启 weston
#   ./push.sh --no-restart # 仅推送，不重启 weston
#   ./push.sh <serial>     # 指定 adb 设备 serial
#
# 推送内容:
#   bin/AndroidAuto            → /data/AndroidAuto/bin/
#   start.sh                   → /data/AndroidAuto/          (launcher 入口，source /opt/x9hp/env.sh 提供 AAC 软解)
#   config/androidauto.ini     → /data/AndroidAuto/config/   (执行目录搜索路径命中)
#   res/icons/androidauto.png  → /data/AndroidAuto/res/icons/
#   ../weston.ini              → /data/weston.ini            (含 icon-id=4007 launcher 入口)

set -e

ADB="adb"
RESTART=1

for arg in "$@"; do
    case "$arg" in
        --no-restart) RESTART=0 ;;
        *) ADB="adb -s $arg" ;;
    esac
done

DEV=/data/AndroidAuto

if [ ! -f bin/AndroidAuto ]; then
    echo "错误: bin/AndroidAuto 不存在，请先执行:"
    echo "  . /home/admin0412/x9sp_wayland/environment-setup-cortexa55-sdrv-linux && make"
    exit 1
fi

echo "[1/5] 创建设备目录"
$ADB shell mkdir -p $DEV/bin $DEV/config $DEV/res/icons $DEV/res/shaders

echo "[2/5] 推送可执行文件与启动脚本"
$ADB push bin/AndroidAuto $DEV/bin/AndroidAuto
$ADB shell chmod +x $DEV/bin/AndroidAuto
$ADB push start.sh $DEV/start.sh
$ADB shell chmod +x $DEV/start.sh

echo "[3/5] 推送配置与资源"
$ADB push config/androidauto.ini $DEV/config/androidauto.ini
$ADB push res/icons/androidauto.png $DEV/res/icons/androidauto.png

echo "[4/5] 推送 weston.ini（launcher 入口）"
$ADB push ../weston.ini /data/weston.ini

echo "[5/5] 重启 weston 生效"
if [ "$RESTART" = "1" ]; then
    $ADB shell /data/start_ivi.sh
else
    echo "  跳过（--no-restart）。注意: weston.ini 变更需重启 weston 才生效。"
fi

echo ""
echo "=== 部署完成 ==="
echo "验证:"
echo "  点击 launcher 上 AndroidAuto 图标启动"
echo "  $ADB shell cat /tmp/androidauto.log     # 触点/生命周期日志"
