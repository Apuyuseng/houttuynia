#!/bin/sh
# Docker Hub 发布脚本（在已完成 docker login 的机器上执行）
# 用法：REG=apuyuseng sh nas/publish-hub.sh
# 依赖：/tmp/crane、/tmp/layer-amd64.tar、/tmp/layer-arm64.tar（已组装）
# ⚠️ 层包 tar 必须在 Linux 容器内打包（如 alpine tar --format=gnu）——
# macOS 的 tar 会把 com.apple.provenance 等 xattr 写进 pax 头，Linux 上的
# Docker 解层时报 lsetxattr "operation not supported"
set -e
REG=${REG:-apuyuseng}
REPO=${REPO:-houttuynia-nas}
C=${CRANE:-/tmp/crane}
export HTTPS_PROXY=${HTTPS_PROXY:-http://127.0.0.1:7897} HTTP_PROXY=${HTTP_PROXY:-http://127.0.0.1:7897}

echo "== 1/4 推送 amd64 =="
$C append -f /tmp/layer-amd64.tar -b debian:bookworm-slim --platform linux/amd64 \
  -t $REG/$REPO:build-amd64

echo "== 2/4 推送 arm64 =="
$C append -f /tmp/layer-arm64.tar -b debian:bookworm-slim --platform linux/arm64 \
  -t $REG/$REPO:build-arm64

echo "== 3/4 注入元数据（ENTRYPOINT/ENV/EXPOSE/VOLUME）=="
# 注：crane 0.20.x 的 mutate 不支持 --expose/--volume（仅元数据装饰，
# 缺失不影响功能；端口/卷在 compose 与文档中声明）
MUTATE_ARGS="--entrypoint /opt/mwine/entrypoint.sh \
  --env HOME=/data --env MWINE_HOST=0.0.0.0 --env MWINE_TLS_PORT=18443 \
  --env MWINE_WEB_DIR=/opt/mwine/web"
$C mutate $REG/$REPO:build-amd64 $MUTATE_ARGS -t $REG/$REPO:build-amd64-cfg
$C mutate $REG/$REPO:build-arm64 $MUTATE_ARGS -t $REG/$REPO:build-arm64-cfg

echo "== 4/4 合成多架构 manifest（latest）=="
$C index append -m $REG/$REPO:build-amd64-cfg -m $REG/$REPO:build-arm64-cfg \
  -t $REG/$REPO:latest
$C index append -m $REG/$REPO:build-amd64-cfg -t $REG/$REPO:amd64
$C index append -m $REG/$REPO:build-arm64-cfg -t $REG/$REPO:arm64

echo "完成：docker.io/$REG/$REPO（latest/amd64/arm64）"
