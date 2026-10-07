#!/bin/bash
# 自建 MWine 镜像组装（2026-10-04 验收通过的配方，在 AOSP 构建 VM 上运行）
# 产物：/tmp/mwine-image/ —— 按 SDK system-image 布局，直接拷入
#   ~/Library/Android/sdk/system-images/android-33/mwine/arm64-v8a/
#   并在 AVD config.ini 设 image.sysdir.1 指向它。
# 前置：AOSP 已完成 sdk_phone_arm64-userdebug 构建（含 emu_img_zip 不必需）
# 依赖：out/host/linux-x86/bin/{mkbootfs,unpack_bootimg}、lz4、cpio
set -euo pipefail

OUT=${OUT:-/home/ubuntu/aosp/out/target/product/emulator_arm64}
HOSTBIN=/home/ubuntu/aosp/out/host/linux-x86/bin
DST=/tmp/mwine-image
W=/tmp/mwine-img-work

rm -rf "$W" "$DST"; mkdir -p "$DST" "$W"/g "$W"/v

echo "[1/5] boot.img generic ramdisk 解包"
"$HOSTBIN/unpack_bootimg" --boot_img "$OUT/boot.img" --out "$W" >/dev/null
(cd "$W/g" && lz4 -dc "$W/ramdisk" | cpio -idm --quiet)

echo "[2/5] init 换成 system 的动态 init（generic ramdisk 自带 bootstrap linker）"
debugfs -R "dump /bin/init $W/init.sys" "$OUT/system.img" 2>/dev/null
[ -s "$W/init.sys" ]
(cd "$W/g" && rm -f init && cp "$W/init.sys" init && chmod 750 init)

echo "[3/5] 合并 vendor_ramdisk（提供 first_stage_ramdisk/fstab.ranchu）"
(cd "$W/v" && { gzip -t "$OUT/vendor_ramdisk.img" 2>/dev/null && gzip -dc "$OUT/vendor_ramdisk.img" || lz4 -dc "$OUT/vendor_ramdisk.img"; } | cpio -idm --quiet)
cp -a "$W/v/." "$W/g/"

echo "[4/5] 打包 ramdisk（cpio newc | lz4 legacy，内核只认 lz4）"
(cd "$W/g" && find . | LC_ALL=C sort | "$HOSTBIN/mkbootfs" . | lz4 -l > "$DST/ramdisk-qemu.img")

echo "[5/5] 组装 SDK 布局（-qemu 真文件 + 规范名符号链接，emulator 读 -qemu 名）"
cp "$OUT/kernel-ranchu"            "$DST/"
cp "$OUT/system-qemu.img"          "$DST/"
cp "$OUT/vendor-qemu.img"          "$DST/"
cp "$OUT/advancedFeatures.ini"     "$DST/"
cp "$OUT/VerifiedBootParams.textproto" "$DST/"
for f in ramdisk system vendor; do ln -sf "$f-qemu.img" "$DST/$f.img"; done
printf 'androidboot.force_normal_boot=1 androidboot.selinux=permissive\n' > "$DST/kernel_cmdline.txt"
# build.prop：emulator 靠它识别 ABI=arm64-v8a 才开 HVF（缺了会静默回退 TCG！）
# 用官方 android-33/default/arm64-v8a/build.prop 克隆即可（宿主侧元数据，品牌以 guest 为准）
echo "!! 记得放入 build.prop（官方 33 克隆）、encryptionkey.img、data/、userdata.img（官方克隆）"

ls -la "$DST"
echo "完成：$DST"
