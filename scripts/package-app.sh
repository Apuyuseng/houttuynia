#!/bin/bash
# MWine.app 打包（快速验证版：完整 SDK emulator，后续优化裁剪）
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="$ROOT/dist/Houttuynia.app"
SDK=~/Library/Android/sdk

echo "=== MWine.app 打包（完整运行时）"

# 镜像与用户数据外置（~/Library/Application Support/MWine/system-images）：
# 不再打进/暂存回 .app——重新打包、替换 .app 都不会丢已导入的镜像与配置。
# bundle 内只放指向外置目录的符号链接（daemon 首启会补建并迁移旧数据）。
IMG_STORE="$HOME/Library/Application Support/MWine/system-images"

rm -rf "$APP"
rm -f "$ROOT"/dist/Houttuynia-20*.zip "$ROOT"/dist/Houttuynia-20*.dmg  # 只清日期包，勿动 runtime 分发包
mkdir -p "$APP/Contents/"{MacOS,Resources}

# Info.plist
cat > "$APP/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key><string>Houttuynia</string>
    <key>CFBundleIdentifier</key><string>com.houttuynia.app</string>
    <key>CFBundleName</key><string>Houttuynia</string>
    <key>CFBundleIconFile</key><string>AppIcon</string>
    <key>CFBundlePackageType</key><string>APPL</string>
    <key>CFBundleShortVersionString</key><string>0.1.0</string>
    <key>LSMinimumSystemVersion</key><string>14.0</string>
    <key>NSHighResolutionCapable</key><true/>
</dict>
</plist>
PLIST

# 可执行文件：Qt 前端
QT_PREFIX="${QT_PREFIX:-$(brew --prefix qtbase)}"
cp "$ROOT/app/qt/build/HouttuyniaApp" "$APP/Contents/MacOS/Houttuynia"
# C++ 守护 + CLI（图标缓存依赖与 mwine-cored 同目录的 mwine-iconfetch 子进程）
cp "$ROOT/cpp/build/daemon/mwine-cored" "$APP/Contents/MacOS/" \
  || { echo "✗ mwine-cored 缺失（先跑 scripts/build-cpp.sh）"; exit 1; }
cp "$ROOT/cpp/build/modules/cli/mwine" "$APP/Contents/MacOS/" \
  || { echo "✗ mwine CLI 缺失（先跑 scripts/build-cpp.sh）"; exit 1; }
# 独立应用窗口播放器（run --window / mirror）
cp "$ROOT/cpp/build/modules/window/mwine-window" "$APP/Contents/MacOS/" \
  || { echo "✗ mwine-window 缺失（先跑 scripts/build-cpp.sh）"; exit 1; }
cp "$ROOT/cpp/build/daemon/mwine-iconfetch" "$APP/Contents/MacOS/" 2>/dev/null \
  || echo "  ⚠ mwine-iconfetch 缺失（应用图标退化为字母块）"

# Qt 动态库部署（LGPLv3：动态链接保持，框架拷入 bundle）。
# 全部可执行先拷齐再统一 macdeployqt：附加二进制（mwine/mwine-window/mwine-cored）
# 必须经 -executable 改写 Qt 依赖路径，否则运行时混载 Homebrew Qt 与 bundle Qt
# 两套库（症状："two sets of Qt binaries" / cocoa 插件初始化失败 → 窗口不弹出）。
QT_SVG_RPATH="$(brew --prefix qtsvg)/lib"
for exe in Houttuynia mwine mwine-window mwine-cored mwine-iconfetch; do
  [ -f "$APP/Contents/MacOS/$exe" ] && \
    install_name_tool -add_rpath "$QT_SVG_RPATH" "$APP/Contents/MacOS/$exe" 2>/dev/null || true
done
DEPLOY_ARGS=(-qmldir="$ROOT/app/qt/qml")
for exe in mwine mwine-window mwine-cored mwine-iconfetch; do
  [ -f "$APP/Contents/MacOS/$exe" ] && DEPLOY_ARGS+=(-executable="$APP/Contents/MacOS/$exe")
done
"$QT_PREFIX/bin/macdeployqt" "$APP" "${DEPLOY_ARGS[@]}"

# macdeployqt 遗留修正：libqsvg/libqsvgicon 以 @rpath/QtSvg 引用、libbrotlidec 以
# @rpath/libbrotlicommon 引用，但它们的 rpath 均不含 bundle Frameworks（本机靠
# Homebrew 路径侥幸解析，干净机器上 SVG 图标/HTTP brotli 会加载失败）——统一补
# @executable_path/../Frameworks rpath（插件随主程序进程加载，@executable_path 即主程序）
for leaky in "$APP/Contents/PlugIns/imageformats/libqsvg.dylib" \
             "$APP/Contents/PlugIns/iconengines/libqsvgicon.dylib" \
             "$APP/Contents/Frameworks/libbrotlidec.1.dylib"; do
  [ -f "$leaky" ] && install_name_tool -add_rpath "@executable_path/../Frameworks" "$leaky" 2>/dev/null || true
done

# 应用自身 QML 模块：开发态靠 exe 旁的 build/Houttuynia 目录解析，
# 部署后该路径不存在（模块未内嵌进 qrc import path）——整体拷入
# Resources/qml（qmldir 为相对路径，连同 qml/ 子目录一并）
mkdir -p "$APP/Contents/Resources/qml/Houttuynia"
cp -R "$ROOT/app/qt/build/Houttuynia/." "$APP/Contents/Resources/qml/Houttuynia/"
mkdir -p "$APP/Contents/Resources/licenses"
cp "$ROOT/app/qt/LICENSE.LGPLv3" "$APP/Contents/Resources/licenses/"
echo "  ✓ Qt 前端 + 全部 Qt 程序动态库部署（LGPLv3 合规：动态链接 + 随附许可文本）"

# 运行时不再打进 .app（首启导入分发包 Houttuynia-runtime-v*.zip 或复用本机 SDK；
# 旧版带运行时的安装会在守护启动时自动迁出到 Application Support/MWine/runtime）。
# 需要"全量离线包"时：MWINE_BUNDLE_RUNTIME=1 bash package-app.sh
RUNTIME="$APP/Contents/Resources/runtime"
mkdir -p "$RUNTIME"
if [ -n "$MWINE_BUNDLE_RUNTIME" ]; then
  cp -R "$SDK/emulator" "$RUNTIME/emulator"
  cp -R "$SDK/platform-tools" "$RUNTIME/platform-tools"
  bash "$ROOT/scripts/trim-runtime.sh" "$RUNTIME"
  echo "  ✓ 全量离线包模式：运行时已内嵌（已裁剪）"
else
  echo "  ✓ 瘦身模式：运行时首启导入/下载（dist/Houttuynia-runtime-v*.zip）"
fi

# 系统镜像外置：符号链接到 Application Support（跨版本/重打包永久保留）
mkdir -p "$IMG_STORE"
ln -s "$IMG_STORE" "$RUNTIME/system-images"
echo "  ✓ 镜像外置于 $IMG_STORE（bundle 内为符号链接）"

# scrcpy server（独立窗口模式）+ companion（设备侧图标渲染）
# v4.1 优先（客户端协议版本 4.1，server 端校验版本号不匹配即退出）
mkdir -p "$APP/Contents/Resources/bin"
cp ~/.mwine/bin/scrcpy-server-v4.1 "$APP/Contents/Resources/bin/" 2>/dev/null \
  || cp ~/.mwine/bin/scrcpy-server-v4.0 "$APP/Contents/Resources/bin/" 2>/dev/null \
  || echo "  ⚠ scrcpy-server 缺失（独立窗口/镜像将不可用）"
cp "$ROOT/assets/companion/dist/mwine-bridge.apk" "$APP/Contents/Resources/bin/" 2>/dev/null \
  || echo "  ⚠ companion mwine-bridge.apk 缺失（矢量图标将退化为默认图标）"
# 流量分析：eBPF 采集器（Apache-2.0，gojue/ecapture android-arm64）
# ELF 可用 upx 压缩（~40MB → ~15MB）。宿主 Mach-O 不可 upx：arm64 macOS 会
# 对自解压存根写可执行内存直接 SIGKILL（实测 ad-hoc 签名+entitlements 也无效）。
cp ~/.mwine/bin/ecapture "$APP/Contents/Resources/bin/" 2>/dev/null \
  || echo "  ⚠ ecapture 缺失（流量分析不可用；放入 ~/.mwine/bin/ecapture 后重新打包）"
if command -v upx >/dev/null 2>&1 && [ -f "$APP/Contents/Resources/bin/ecapture" ]; then
  upx --lzma -q "$APP/Contents/Resources/bin/ecapture" >/dev/null 2>&1 \
    && echo "  ✓ ecapture 已 upx 压缩 $(du -h "$APP/Contents/Resources/bin/ecapture" | cut -f1)" \
    || echo "  ⚠ upx 压缩 ecapture 失败（保留原样）"
fi

# AVD 模板（零依赖首启种子：只带配置，userdata 全新）——放基础包
# Resources/avd-template（supervisor 两处都会找：基础包优先，运行时根兜底）
AVD_SRC=$(ls -d ~/Library/Application\ Support/MWine/avd/Android33-Houttuynia.avd \
              ~/.android/avd/Android33-Houttuynia.avd \
              ~/.android/avd/MWine4.avd 2>/dev/null | head -1)
mkdir -p "$APP/Contents/Resources/avd-template"
# gpu 归 auto（swiftshader_indirect 写法会被启动器改写，语义也已不需要）
# sysdir 对齐「导入分发包」的默认安装名（houttuynia）
if [ -n "$AVD_SRC" ] && [ -f "$APP/Contents/Resources/avd-template/config.ini" ]; then
  sed -i "" "s/^hw.gpu.mode *=.*/hw.gpu.mode = auto/" "$APP/Contents/Resources/avd-template/config.ini" 2>/dev/null
  sed -i "" "s|^image.sysdir.1 = .*|image.sysdir.1 = system-images/android-33/houttuynia/arm64-v8a/|" "$APP/Contents/Resources/avd-template/config.ini" 2>/dev/null
fi

# build-tools：运行时只需 aapt2（壳生成的 badging 解析）；
# d8/apksigner/android.jar 是 companion APK 构建工具（scripts/build_companion.sh，
# 开发机执行），打进用户包纯属浪费（合计 ~34M）
BT_VER=$(ls "$SDK/build-tools" | sort -V | tail -1)
mkdir -p "$RUNTIME/build-tools/$BT_VER"
cp "$SDK/build-tools/$BT_VER/aapt2" "$RUNTIME/build-tools/$BT_VER/" 2>/dev/null \
  || echo "  ⚠ aapt2 缺失（.app 壳生成将退化）"
echo "  ✓ avd 模板 + aapt2"

# 体积优化注意：Qt Quick Controls 的风格插件在 Resources/qml/QtQuick/Controls/<Style>/
# 有 qmldir 引用且依赖对应 Frameworks 框架——引擎启动时会加载它们；三处（qml 目录/
# PlugIns/Frameworks）不同步删除会导致 Controls 模块整体加载失败。故风格不裁剪，
# 只做无引用风险的 QML 编译缓存清理（按需重建）。
find "$APP/Contents/Resources/qml" \( -name '*.qmlc' -o -name '*.jsc' \) -delete 2>/dev/null
echo "  ✓ qmlc 清理"

# 图标（鱼腥草心形叶）
mkdir -p "$APP/Contents/Resources"
cp "$ROOT/assets/icons/AppIcon.icns" "$APP/Contents/Resources/AppIcon.icns" 2>/dev/null ||   echo "  ⚠ assets/icons/AppIcon.icns 缺失（图标留空）"
# 签名（--deep：macdeployqt 改写过框架/dylib 的 install name，需整体重签）
codesign -s - --force --deep "$APP" 2>/dev/null || true

echo "=== 完成"
du -sh "$APP"

# 首启修复脚本：清除 Gatekeeper 隔离标记（ad-hoc 签名应用在新机器上会被误报"已损坏"）
cat > "$ROOT/dist/修复打开权限.command" <<'CMD'
#!/bin/bash
# 双击运行：清除 Houttuynia.app 的隔离标记并移入应用程序目录
cd "$(dirname "$0")"
APP=$(ls -d ./Houttuynia.app 2>/dev/null | head -1)
[ -z "$APP" ] && APP="/Applications/Houttuynia.app"
if [ ! -d "$APP" ]; then echo "未找到 Houttuynia.app（把它和本文件放同一目录）"; read -r -p "按回车退出"; exit 1; fi
xattr -cr "$APP"
if [ "$APP" = "./Houttuynia.app" ] && [ -d /Applications ]; then
  rm -rf "/Applications/Houttuynia.app"
  cp -R "$APP" /Applications/ && APP="/Applications/Houttuynia.app"
fi
echo "✓ 已修复，正在打开 Houttuynia…"
open "$APP"
read -r -p "按回车关闭窗口"
CMD
chmod +x "$ROOT/dist/修复打开权限.command"

# 分发压缩（ditto zip：保留 symlink/权限；hdiutil 对稀疏镜像文件会误报 ENOSPC）
ZIP="$ROOT/dist/Houttuynia-$(date +%Y%m%d).zip"
rm -f "$ZIP"
ditto -c -k --keepParent "$APP" "$ZIP"
echo "ZIP: $ZIP"
du -sh "$ZIP"
