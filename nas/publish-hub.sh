#!/bin/sh
# Docker Hub 发布脚本：buildx 多架构直推（macOS 上执行）
# 用法：sh nas/publish-hub.sh
# 前置：① docker login 已登录 Docker Hub；② docker daemon 网络可达 Docker Hub
#       （走守护进程网络，不依赖本机代理）
#
# ⚠️ 历史坑（勿回退）：旧流程「macOS tar 手搓层包 + crane append」中 bsdtar
# 会把 com.apple.provenance 等 xattr 写进 pax 头，Linux Docker 解层报
# lsetxattr "operation not supported"（DockerHub 发布已踩坑两次）。
# 新流程用 buildx 本地上下文构建——BuildKit 不传输文件 xattr，结构性消除
# 该问题；推送后仍拉回扫描层 tar 复核（0 xattr 头才算成功）。
#
# adb 说明：官方 platform-tools 仅有 linux-amd64；arm64 无官方静态版
# （Debian android-tools-adb 需 glibc>=2.38 + 全套 android 私有库，
# bookworm-slim 基座带不动）→ arm64 镜像不带 adb，与既往发布一致。
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)
REG=${REG:-apuyuseng}
REPO=${REPO:-houttuynia-nas}
CTX=${CTX:-/tmp/hubctx}
ADB_AMD64=${ADB_AMD64:-/tmp/adb-bins/adb-amd64}

echo "== 1/6 容器内交叉编译网关（musl 静态；amd64 走 Rosetta/QEMU 较慢）=="
mkdir -p nas/rust/dist-out
[ -f nas/rust/dist-out/mwine-nas-arm64 ] || docker run --rm \
  -v "$ROOT/nas/rust:/src" -w /src -e CARGO_TARGET_DIR=/tmp/t rust:alpine \
  sh -c "sed -i 's#dl-cdn.alpinelinux.org#mirrors.ustc.edu.cn#g' /etc/apk/repositories \
      && apk add --no-cache musl-dev gcc >/dev/null \
      && cargo build --release && cp /tmp/t/release/mwine-nas /src/dist-out/mwine-nas-arm64"
[ -f nas/rust/dist-out/mwine-nas-amd64 ] || docker run --rm --platform linux/amd64 \
  -v "$ROOT/nas/rust:/src" -w /src -e CARGO_TARGET_DIR=/tmp/t rust:alpine \
  sh -c "sed -i 's#dl-cdn.alpinelinux.org#mirrors.ustc.edu.cn#g' /etc/apk/repositories \
      && apk add --no-cache musl-dev gcc >/dev/null \
      && cargo build --release && cp /tmp/t/release/mwine-nas /src/dist-out/mwine-nas-amd64"

echo "== 2/6 组装构建上下文 =="
rm -rf "$CTX" && mkdir -p "$CTX/bin" "$CTX/arch/amd64" "$CTX/arch/arm64" "$CTX/web"
cp nas/Dockerfile.hub "$CTX/Dockerfile"
cp -R web-vue/dist/ "$CTX/web/"
cp dist/Houttuynia.app/Contents/Resources/bin/scrcpy-server-v4.1 "$CTX/bin/"
cp assets/companion/dist/mwine-bridge.apk "$CTX/bin/"
cp nas/entrypoint.hub.sh "$CTX/entrypoint.sh"
cp nas/rust/dist-out/mwine-nas-arm64 "$CTX/"
cp nas/rust/dist-out/mwine-nas-amd64 "$CTX/"
cp "$ADB_AMD64" "$CTX/arch/amd64/adb"
touch "$CTX/arch/arm64/.keep"

echo "== 3/6 清理上下文 xattr（尽力而为；buildx 本就不传 xattr，双保险）=="
XATTR=$(command -v /usr/bin/xattr || true)
[ -n "$XATTR" ] && $XATTR -rc "$CTX" 2>/dev/null || echo "（xattr 清理跳过——依赖 buildx 不传输 xattr）"

echo "== 4/6 构建 + 推送（latest 多架构 / amd64 / arm64 单架构标签）=="
docker buildx use hubbuild-np 2>/dev/null || docker buildx create --name hubbuild-np --driver docker-container --use
cd "$CTX"
docker buildx build --platform linux/amd64,linux/arm64 -f Dockerfile \
  -t $REG/$REPO:latest --push .
docker buildx build --platform linux/amd64 -f Dockerfile \
  -t $REG/$REPO:amd64 --push .
docker buildx build --platform linux/arm64 -f Dockerfile \
  -t $REG/$REPO:arm64 --push .
cd "$ROOT"

echo "== 5/6 拉回复核（解层即历史故障现场：lsetxattr 报错在此复现）=="
docker pull -q $REG/$REPO:latest

echo "== 6/6 扫描层 tar 中的 xattr 头（0 才算过）=="
docker save $REG/$REPO:latest -o /tmp/hub-verify.tar
python3 - <<'EOF'
import tarfile, sys
bad = 0
with tarfile.open('/tmp/hub-verify.tar') as img:
    layers = [m.name for m in img.getmembers() if m.name.endswith(('.tar', '/layer.tar'))]
    for ln in layers:
        f = img.extractfile(ln)
        with tarfile.open(fileobj=f) as lt:
            for m in lt:
                xa = {k: v for k, v in (m.pax_headers or {}).items()
                      if 'xattr' in k or 'com.apple' in k or 'SCHILY' in k}
                if xa:
                    bad += 1
                    print(f"  ✗ {ln}!{m.name}: {xa}")
print("✓ 层内无 xattr 头" if bad == 0 else f"✗ 发现 {bad} 个带 xattr 头的成员")
sys.exit(1 if bad else 0)
EOF
echo "完成：docker.io/$REG/$REPO（latest/amd64/arm64）"
