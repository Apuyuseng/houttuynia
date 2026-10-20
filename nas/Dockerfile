# syntax=docker/dockerfile:1
# ============================================================
# 鱼腥草（Houttuynia）NAS 版 —— 多合一镜像
#
#   docker build -t houttuynia-nas -f nas/Dockerfile .
#
# 构建产物：mwine-cored 守护进程（REST + Web 控制台）
# 运行时需要 Android SDK 模拟器（容器内不包含，见 nas/setup-emulator.sh
# 或 docs/NAS.md「模拟器运行时」一节）。
# ============================================================

# ---------- 构建阶段 ----------
FROM debian:bookworm AS build

# 国内构建建议：--build-arg APT_MIRROR=mirrors.ustc.edu.cn
# （deb.debian.org 直连在部分网络只有几十 KB/s；置空走默认官方源）
ARG APT_MIRROR=""
RUN if [ -n "$APT_MIRROR" ]; then \
        sed -i "s|http://deb.debian.org|http://${APT_MIRROR}|g" \
            /etc/apt/sources.list.d/debian.sources; \
    fi \
    && apt-get update && apt-get install -y --no-install-recommends \
        cmake g++ make ninja-build \
        qt6-base-dev qt6-base-dev-tools \
        libzstd-dev libsqlite3-dev libssl-dev zlib1g-dev \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY cpp/ ./cpp/

# 无头构建：只编译守护进程依赖的模块（跳过窗口/音频/输入等桌面模块）
RUN cmake -S cpp -B cpp/build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DMWINE_HEADLESS=ON \
    && cmake --build cpp/build --target mwine-cored -j"$(nproc)" \
    && strip cpp/build/daemon/mwine-cored

# ---------- 运行阶段 ----------
FROM debian:bookworm-slim

ARG APT_MIRROR=""
RUN if [ -n "$APT_MIRROR" ]; then \
        sed -i "s|http://deb.debian.org|http://${APT_MIRROR}|g" \
            /etc/apt/sources.list.d/debian.sources; \
    fi \
    && apt-get update && apt-get install -y --no-install-recommends \
        libqt6core6 libqt6network6 \
        libzstd1 libsqlite3-0 libssl3 zlib1g \
        ca-certificates curl unzip xz-utils \
        procps net-tools \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/cpp/build/daemon/mwine-cored /usr/local/bin/mwine-cored
COPY web/ /opt/mwine/web/
COPY nas/setup-emulator.sh /opt/mwine/setup-emulator.sh
RUN chmod +x /opt/mwine/setup-emulator.sh

# HOME=/data：~/.mwine（配置/日志/上传）、~/.android（AVD）、
# ~/.local/share/MWine（运行时/镜像）全部落在持久卷里
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

ENTRYPOINT ["/usr/local/bin/mwine-cored"]
