#!/bin/sh
# 自含入口：adb 密钥落卷（首启；设备已授权该钥，免重新弹窗）→ 容器内起
# adb server（host 网络 → 绑宿主 5037）→ exec 网关主进程。
set -e
mkdir -p /data/.android
if [ ! -f /data/.android/adbkey ]; then
    cp /opt/mwine/keys/adbkey /data/.android/adbkey
    cp /opt/mwine/keys/adbkey.pub /data/.android/adbkey.pub
    chmod 600 /data/.android/adbkey
fi
/opt/mwine/adb start-server
exec /usr/local/bin/mwine-nas
