#!/bin/bash
# 运行时裁剪：删掉产品路线（lavapipe 直通 + gRPC 控制）用不到的组件。
# 依据（2026-10 实测，冷启动回归验证）：
#  - ⚠ gles_angle 与 libvk_swiftshader 必须保留：GLES 路线是 swangle
#    （ANGLE→SwiftShader-Vulkan），两者缺一都会翻车——删 gles_angle 启动即报
#    "could not configure renderer"；删 libvk_swiftshader 启动正常但 guest 画面
#    冻结（无帧产出，表现为输入全部"无效"）。libz3/glslangValidator 确认可删
#  - qemu-system-armel：32 位 ARM 模拟，产品只跑 arm64
#  - qt/translations、qtwebengine_locales、devtools 资源：多语言/调试资源，懒加载
#  - vulkan 的 z3/glslangValidator：LLVM 可选依赖与命令行工具，无 otool 引用
#  - qsn 皮肤：qemu 主程序 0 引用（extended window 不走皮肤）
#  - lib/{cmake,pkgconfig,*.a}：开发期产物
# 保留（勿动！曾想删但确认必需）：
#  - libLLVM.dylib + libvulkan_lvp.dylib：lavapipe 命脉
#  - libQt6WebEngine*：qemu-system-aarch64 直接链接（macOS dylib 启动即加载）
#  - icudtl.dat / qtwebengine_resources.pak：WebEngine 初始化保险
#  - emulator-check：emulator 启动器引用
#  - bin64（ext4 工具）：userdata 镜像格式化
#  - netsimd / nimble_bridge / resources / pc-bios / proto
# 用法: trim-runtime.sh <runtime-dir>   # runtime-dir = .../Contents/Resources/runtime
set -e
RT="${1:?用法: trim-runtime.sh <runtime-dir>}"
EMU="$RT/emulator"

rm -f  "$EMU/qemu/darwin-aarch64/qemu-system-armel" \
       "$EMU/qemu/darwin-aarch64/qemu-system-armel-headless"
rm -rf "$EMU/lib64/qt/translations" \
       "$EMU/lib64/qt/libexec/qtwebengine_locales"
rm -f  "$EMU/lib64/qt/libexec/qtwebengine_devtools_resources.pak"
rm -f  "$EMU/lib64/vulkan/libz3.4.15.dylib" \
       "$EMU/lib64/vulkan/glslangValidator"
rm -rf "$EMU/qsn" "$EMU/crashreport" "$EMU/include" \
       "$EMU/lib/cmake" "$EMU/lib/pkgconfig"
rm -f  "$EMU/lib/libflatbuffers.a"

# platform-tools：只留 adb 及其运行件
PT="$RT/platform-tools"
rm -f  "$PT/fastboot" "$PT/etc1tool" "$PT/hprof-conv" \
       "$PT/make_f2fs" "$PT/make_f2fs_casefold" \
       "$PT/mke2fs" "$PT/mke2fs.conf" "$PT/sqlite3" \
       "$PT/package.xml" "$PT/source.properties" 2>/dev/null || true

echo "✓ 裁剪完成: $(du -sh "$RT" | cut -f1)"
