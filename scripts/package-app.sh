#!/bin/bash
# MWine.app 打包（快速验证版：完整 SDK emulator，后续优化裁剪）
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="$ROOT/dist/Houttuynia.app"
SDK=~/Library/Android/sdk

echo "=== MWine.app 打包（完整运行时）"
rm -rf "$APP"
rm -f "$ROOT"/dist/Houttuynia-*.zip "$ROOT"/dist/Houttuynia-*.dmg
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

# 可执行文件
cp "$ROOT/app/MWineUI/.build/release/MWineUI" "$APP/Contents/MacOS/Houttuynia"
cp "$ROOT/target/release/mwine-core" "$APP/Contents/MacOS/"
cp "$ROOT/target/release/mwine" "$APP/Contents/MacOS/"

# 完整 SDK emulator（含 Qt/vulkan/gles_angle/全部工具）
RUNTIME="$APP/Contents/Resources/runtime"
mkdir -p "$RUNTIME"
cp -R "$SDK/emulator" "$RUNTIME/emulator"
cp -R "$SDK/platform-tools" "$RUNTIME/platform-tools"
echo "  ✓ 完整 SDK emulator + platform-tools"

# 系统镜像不打包：用户在「镜像」页导入分发包。
# 模板的 sysdir 指向导入后的默认名（android-33/houttuynia），导入完即可直接启动。
mkdir -p "$RUNTIME/system-images"
echo "  ✓ 镜像位留空（随分发包 houttuynia-image-v1.0.2-*.tar.zst 分发，导入后可用）"

# scrcpy server（独立窗口模式）+ 按键注入器
mkdir -p "$APP/Contents/Resources/bin"
cp ~/.mwine/bin/scrcpy-server-v4.0 "$APP/Contents/Resources/bin/" 2>/dev/null
cp "$ROOT/app/KeyTap/.build/release/MWineKeys" "$APP/Contents/Resources/bin/"
echo "  ✓ MWineKeys 注入器"

# AVD 模板（零依赖首启种子：只带配置，userdata 全新）
AVD_SRC=$(ls -d ~/Library/Application\ Support/MWine/avd/Android33-Houttuynia.avd \
              ~/.android/avd/Android33-Houttuynia.avd \
              ~/.android/avd/MWine4.avd 2>/dev/null | head -1)
mkdir -p "$RUNTIME/avd-template"
[ -n "$AVD_SRC" ] && cp "$AVD_SRC/config.ini" "$RUNTIME/avd-template/"
# gpu 归 auto（swiftshader_indirect 写法会被启动器改写，语义也已不需要）
sed -i "" "s/^hw.gpu.mode *=.*/hw.gpu.mode = auto/" "$RUNTIME/avd-template/config.ini" 2>/dev/null
# sysdir 对齐「导入分发包」的默认安装名（houttuynia）
sed -i "" "s|^image.sysdir.1 = .*|image.sysdir.1 = system-images/android-33/houttuynia/arm64-v8a/|" "$RUNTIME/avd-template/config.ini" 2>/dev/null

# build-tools（aapt2：安装 APK 生成 .app 壳需要）
BT_VER=$(ls "$SDK/build-tools" | sort -V | tail -1)
mkdir -p "$RUNTIME/build-tools/$BT_VER"
cp "$SDK/build-tools/$BT_VER"/{aapt2,d8,apksigner} "$RUNTIME/build-tools/$BT_VER/" 2>/dev/null
cp "$SDK/platforms/android-33/android.jar" "$RUNTIME/platforms/android-33/" 2>/dev/null || {
  mkdir -p "$RUNTIME/platforms/android-33"
  cp "$SDK/platforms/android-33/android.jar" "$RUNTIME/platforms/android-33/"
}
echo "  ✓ avd 模板 + build-tools"

# 图标（鱼腥草心形叶）
mkdir -p "$APP/Contents/Resources"
cp "$ROOT/assets/icons/AppIcon.icns" "$APP/Contents/Resources/AppIcon.icns" 2>/dev/null ||   echo "  ⚠ assets/icons/AppIcon.icns 缺失（图标留空）"
# 签名
codesign -s - --force "$APP" 2>/dev/null || true

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
