#!/bin/sh
# 在容器（或 Linux 主机）内安装 Android SDK 模拟器运行时到持久卷。
# 用法：docker exec -it <容器> /opt/mwine/setup-emulator.sh
#
# 仅 linux/x86_64 有官方模拟器构建；linux/arm64 暂无官方构建
# （ARM 主机请自行编译 emulator 或改用 Mac 服务器模式，见 docs/NAS.md）。
set -e

SDK_ROOT="${ANDROID_HOME:-$HOME/android-sdk}"
ARCH="$(uname -m)"

if [ "$ARCH" != "x86_64" ]; then
    echo "⚠ 当前架构 $ARCH：Google 未发布 linux-$ARCH 版模拟器。"
    echo "  可只使用 Web 控制台管理（VM 启动需自行提供 emulator 并设置 MWINE_SDK_ROOT）。"
fi

CMDTOOLS_VER="11076708"
URL="https://dl.google.com/android/repository/commandlinetools-linux-${CMDTOOLS_VER}_latest.zip"

echo "==> 安装目录: $SDK_ROOT"
mkdir -p "$SDK_ROOT/cmdline-tools"
cd "$SDK_ROOT"

if [ ! -x "cmdline-tools/latest/bin/sdkmanager" ]; then
    echo "==> 下载命令行工具…"
    curl -fSL "$URL" -o /tmp/cmdtools.zip
    rm -rf /tmp/cmdtools-extract cmdline-tools/latest
    mkdir -p /tmp/cmdtools-extract
    unzip -q /tmp/cmdtools.zip -d /tmp/cmdtools-extract
    mv /tmp/cmdtools-extract/cmdline-tools cmdline-tools/latest
    rm -rf /tmp/cmdtools.zip /tmp/cmdtools-extract
fi

export ANDROID_HOME="$SDK_ROOT"
yes | cmdline-tools/latest/bin/sdkmanager --licenses > /dev/null 2>&1 || true

echo "==> 安装 platform-tools 与 emulator…"
cmdline-tools/latest/bin/sdkmanager "platform-tools" "emulator"

echo ""
echo "✓ 运行时安装完成：$SDK_ROOT"
echo "  容器启动参数加上：  -e MWINE_SDK_ROOT=$SDK_ROOT"
echo "  （compose 已在 environment 预留，取消注释即可）"
echo ""
echo "提示：系统镜像请在 Web 控制台「镜像」页在线拉取或导入分发包。"
