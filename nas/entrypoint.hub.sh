#!/bin/sh
# 自含入口：adb server（容器内，host 网络绑宿主 5037）→ 网关主进程。
# 密钥：/data/.android 无密钥时自动生成（adb start-server 首次运行自动
# 生成），用户在设备上完成一次调试授权即可；私有部署可将已授权的
# adbkey{,.pub} 预放到 /data/.android/ 跳过授权。
set -e
# adb server 不再需要：网关内置集成传输层（Rust 直驱 usbfs/TCP），
# 与设备之间无任何中间进程。
exec /usr/local/bin/mwine-nas
