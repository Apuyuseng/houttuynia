# ============================================================
# 鱼腥草（Houttuynia）NAS 版 —— Rust 网关 + Vue 前端 多合一镜像
#
#   docker build -t houttuynia-nas -f nas/Dockerfile .
#
# 构建产物：mwine-nas（Rust，axum + 自研 adb/scrcpy 协议栈，无 Qt 依赖）
# 前端：web-vue/dist 预构建产物（随源码分发，构建阶段无需 node）
# 模拟器运行时不随镜像分发（容器内跑 nas/setup-emulator.sh 安装，见 nas/README.md）。
# ============================================================

# ---------- Rust 构建阶段 ----------
FROM rust:1-bookworm AS build

WORKDIR /src
COPY nas/rust/ ./

# 国内 crates 拉取建议加 --build-arg CARGO_MIRROR=rsproxy
ARG CARGO_MIRROR=""
RUN if [ -n "$CARGO_MIRROR" ]; then \
        mkdir -p "$CARGO_HOME" && printf '%s\n' \
        '[source.crates-io]' 'replace-with = "rsproxy-sparse"' \
        '[source.rsproxy-sparse]' 'registry = "sparse+https://rsproxy.cn/index/"' \
        '[registries.rsproxy]' 'index = "sparse+https://rsproxy.cn/index/"' \
        '[net]' 'git-fetch-with-cli = true' > "$CARGO_HOME/config.toml"; \
    fi \
    && cargo build --release \
    && strip target/release/mwine-nas

# ---------- 运行阶段（纯静态二进制 + 预构建前端，无 Qt）----------
FROM debian:bookworm-slim

# 国内构建建议：--build-arg APT_MIRROR=mirrors.ustc.edu.cn
ARG APT_MIRROR=""
RUN if [ -n "$APT_MIRROR" ]; then \
        sed -i "s|http://deb.debian.org|http://${APT_MIRROR}|g" \
            /etc/apt/sources.list.d/debian.sources; \
    fi \
    && apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl unzip xz-utils \
        procps net-tools \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/target/release/mwine-nas /usr/local/bin/mwine-nas
COPY web-vue/dist/ /opt/mwine/web/
COPY nas/setup-emulator.sh /opt/mwine/setup-emulator.sh
COPY assets/companion/dist/mwine-bridge.apk /opt/mwine/bin/mwine-bridge.apk
RUN chmod +x /opt/mwine/setup-emulator.sh

# HOME=/data：~/.mwine（配置/日志/上传）、~/.android（AVD）全部落在持久卷
VOLUME /data
ENV HOME=/data \
    MWINE_HOST=0.0.0.0 \
    MWINE_PORT=7788 \
    MWINE_WEB_DIR=/opt/mwine/web \
    MWINE_HEADLESS=1
# MWINE_TOKEN：务必在启动时注入（-e MWINE_TOKEN=xxx 或 compose environment）

EXPOSE 7788
HEALTHCHECK --interval=30s --timeout=5s --retries=3 \
    CMD curl -fsS http://127.0.0.1:${MWINE_PORT}/health || exit 1

ENTRYPOINT ["/usr/local/bin/mwine-nas"]
