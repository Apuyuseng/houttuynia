#!/usr/bin/env bash
# userdebug 镜像一键回拉与集成（2026-10-11）
# 前置：VM 上 build-mwine-image.sh 已在 /tmp/mwine-image 组装完毕
# 动作：zstd 压缩回拉 → 组装新 sysdir → 备份旧镜像 → 原子替换 → 清 AVD 覆盖层
set -euo pipefail

VM=192.168.31.139; VPORT=2224; VUSER=ubuntu
SSH="ssh -o BatchMode=yes -p $VPORT $VUSER@$VM"
RT=~/Applications/Houttuynia/Houttuynia.app/Contents/Resources/runtime
CUR="$RT/system-images/android-33/houttuynia/arm64-v8a"
NEW="$RT/system-images/android-33/houttuynia/arm64-v8a-new"
BAK="$RT/system-images/android-33/houttuynia/arm64-v8a-user-bak"
WORK=/tmp/mwine-img-fetch

echo "[1/6] 检查 VM 组装产物"
$SSH 'ls -la /tmp/mwine-image/ | head -12' | tee /tmp/vm-img-list.txt
$SSH 'test -s /tmp/mwine-image/system-qemu.img' || { echo "VM 侧 /tmp/mwine-image 未就绪（先跑 build-mwine-image.sh）"; exit 1; }

echo "[2/6] VM 侧压缩"
rm -rf "$WORK"; mkdir -p "$WORK"
$SSH 'cd /tmp/mwine-image && rm -f /tmp/mw-pkg.tar.zst && tar cf - system-qemu.img vendor-qemu.img ramdisk-qemu.img kernel-ranchu advancedFeatures.ini VerifiedBootParams.textproto | zstd -T4 -19 -o /tmp/mw-pkg.tar.zst && sha256sum /tmp/mw-pkg.tar.zst'

echo "[3/6] 回拉（直连 ssh，密钥认证）"
$SSH 'cat /tmp/mw-pkg.tar.zst' > "$WORK/mw-pkg.tar.zst"
shasum -a 256 "$WORK/mw-pkg.tar.zst"

echo "[4/6] 解包组装新 sysdir"
mkdir -p "$NEW"
tar -I zstd -xf "$WORK/mw-pkg.tar.zst" -C "$NEW"
# 官方克隆件与元数据从现目录复用（HVF 识别/加密/用户数据与镜像内容无关）
for f in build.prop encryptionkey.img userdata.img manifest.json source.properties data; do
  [ -e "$CUR/$f" ] && cp -R "$CUR/$f" "$NEW/$f"
done
for f in ramdisk system vendor; do ln -sf "$f-qemu.img" "$NEW/$f.img"; done

echo "[5/6] 备份旧镜像并原子替换"
[ -d "$BAK" ] || mv "$CUR" "$BAK"
mv "$NEW" "$CUR"

echo "[6/6] 清 AVD 覆盖层（否则 qcow2 掩蔽新镜像）"
AVD=~/"Library/Application Support/MWine/avd/Android33-Houttuynia.avd"
ls "$AVD"/*.qcow2 2>/dev/null && rm -f "$AVD"/*.qcow2 && echo "已清 qcow2"
rm -f "$AVD"/cache.img.qcow2 "$AVD"/userdata-qemu.img.qcow2 2>/dev/null || true

echo "完成。验收顺序：启动 VM → getprop ro.build.type=userdebug → adb root → which tcpdump"
