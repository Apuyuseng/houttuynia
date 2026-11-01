# 鱼腥草（Houttuynia）NAS 版 —— Docker Hub 镜像说明

> 中文说明见下方；English summary at the bottom.

<p align="center">
  <img src="https://raw.githubusercontent.com/Apuyuseng/houttuynia/nas/assets/logo/logo.svg" width="120" alt="Houttuynia">
</p>

# Houttuynia NAS（鱼腥草）

**网页控制的 Android 设备网关** —— 在 NAS / 家庭服务器上运行，局域网内任何浏览器（手机/平板/电脑）都能管理局域网或 USB 连接的 Android 设备（手机 / 电视 / 盒子）。

- **投屏**：scrcpy 协议 + WebCodecs 硬解，带音频，触控/按键/滚轮/文本注入
- **应用库**：真实图标（APK 内提取）、安装 / 卸载 / 主屏启动 / 独立窗口
- **文件管理**：设备 `/sdcard` 浏览 / 上传 / 下载 / 重命名 / 删除（全流式，大文件内存恒定）
- **家长管控**：屏幕状态检测、允许时段、每日亮屏上限、应用黑名单、外网封禁
- **ADB 终端**、截图、设备壁纸、电量 / 温度 / 性能监控
- **多设备**：USB 直连（插入即连）+ 无线（扫码配对 / 配对码 / 手动 IP），历史连接一键回连
- 纯 Rust 自研 adb / scrcpy 协议栈，**零外部依赖**（无 Qt、无 Android SDK、宿主机无需装 adb）

## 快速开始

```bash
docker run -d \
  --name houttuynia-nas \
  --restart unless-stopped \
  --network host \
  -v /dev/bus/usb:/dev/bus/usb \
  --device-cgroup-rule "c 189:* rmw" \
  -e MWINE_TOKEN=改成你的强密码 \
  -e MWINE_TLS_PORT=18443 \
  -v houttuynia-data:/data \
  ghcr.io/apuyuseng/houttuynia-nas:latest
```

打开 **https://NAS的IP:18443**（自签证书，浏览器接受一次即可），输入 `MWINE_TOKEN` 登录。

> **为什么必须 https？** 投屏使用 WebCodecs，只在安全上下文（HTTPS）可用。
> 证书为首次启动自动生成的自签证书，持久保存在数据卷，重启不变。

## 标签（Tags）

| 标签 | 说明 |
|---|---|
| `latest` | 最新稳定版，linux/amd64 + linux/arm64 双架构 |
| `x.y.z` | 版本号归档 |

## 参数

| 变量 | 默认 | 说明 |
|---|---|---|
| `MWINE_TOKEN` | 空 | **必设**。访问令牌（Web 登录密码）；不设则局域网任何人可控制设备 |
| `MWINE_TLS_PORT` | `18443` | HTTPS 端口（host 网络，端口直接落宿主机） |
| `MWINE_HOST` | `0.0.0.0` | 绑定地址 |
| `MWINE_WEB_DIR` | `/opt/mwine/web` | 前端目录（已内置） |

数据（配置 / 图标缓存 / 管控数据库 / adb 密钥）全部落在 `/data` 卷。

## 设备连接

- **USB**：数据线插到 NAS 即出现在「真机」页（首次需在设备上允许 USB 调试授权）
- **无线**：「真机」页 → 连接抽屉 → 扫码配对 / 配对码 / 手动 IP:端口
  （设备需开启「无线调试」，Android 11+ 支持扫码）
- **多设备**：设备表里「设为当前」切换管理目标

## 常见问题

- **打不开页面 / 投屏不可用**：确认走 `https://`；首次访问接受自签证书
- **应用列表为空**：确认设备已连接（`真机` 页）；无线设备需开启「无线调试」
- **USB 设备不显示**：容器需要 `/dev/bus/usb` 挂载与 `--device-cgroup-rule`（见上方命令）；`network_mode: host` 为必需（mDNS 发现与 5037 端口共享）
- **linux/arm64**：无官方模拟器，本镜像为**纯真机网关**不受影响

## docker compose

```yaml
services:
  houttuynia-nas:
    image: ghcr.io/apuyuseng/houttuynia-nas:latest
    container_name: houttuynia-nas
    restart: unless-stopped
    network_mode: host
    environment:
      MWINE_TOKEN: "please-change-me"
      MWINE_TLS_PORT: "18443"
    volumes:
      - /dev/bus/usb:/dev/bus/usb
      - houttuynia-data:/data
    devices:
      - /dev/bus/usb:/dev/bus/usb
    # device_cgroup_rules 在 compose 顶层：
    # device_cgroup_rules:
    #   - "c 189:* rmw"
volumes:
  houttuynia-data:
```

## 源码

https://github.com/Apuyuseng/houttuynia（`nas` 分支）

---

# Houttuynia NAS (English)

**Web-controlled Android device gateway** for your NAS / home server. Any browser on the LAN can manage Android devices (phones / TVs / boxes) connected via USB or Wi-Fi.

- **Screen mirroring**: scrcpy protocol + WebCodecs hardware decoding, with audio, touch / key / scroll / text injection
- **App library**: real icons extracted from APKs, install / uninstall / launch / standalone window
- **File manager**: browse / upload / download / rename / delete on `/sdcard` (fully streamed)
- **Parental controls**: screen state detection, allowed time windows, daily screen-time limit, app blocklist, internet blocking
- **ADB terminal**, screenshots, device wallpaper, battery / temperature / performance monitoring
- **Multi-device**: USB plug-and-play + wireless (QR pairing / pairing code / manual IP), one-click reconnect from history
- Pure-Rust adb / scrcpy protocol stack — the container bundles adb; **the host needs nothing installed**

## Quick start

```bash
docker run -d \
  --name houttuynia-nas \
  --restart unless-stopped \
  --network host \
  -v /dev/bus/usb:/dev/bus/usb \
  --device-cgroup-rule "c 189:* rmw" \
  -e MWINE_TOKEN=change-me-to-a-strong-password \
  -e MWINE_TLS_PORT=18443 \
  -v houttuynia-data:/data \
  ghcr.io/apuyuseng/houttuynia-nas:latest
```

Open **https://NAS-IP:18443** (self-signed certificate — accept it once), log in with `MWINE_TOKEN`.

> **Why HTTPS?** Screen mirroring uses WebCodecs, which is only available in secure
> contexts. A self-signed certificate is generated on first start and persisted in
> the data volume.

## Environment

| Variable | Default | Description |
|---|---|---|
| `MWINE_TOKEN` | empty | **Required.** Access token (web login); unset = anyone on the LAN can control devices |
| `MWINE_TLS_PORT` | `18443` | HTTPS port (host network — port lands directly on the host) |

All state (config / icon cache / parental-control DB / adb keys) lives in the `/data` volume.

## Connecting devices

- **USB**: plug the cable into the NAS — the device appears on the *Devices* page (approve the debugging prompt once)
- **Wi-Fi**: *Devices* page → *Connect* drawer → QR pairing / pairing code / manual IP:port (enable *Wireless debugging* on the device)
- **Multiple devices**: use *Set as current* to switch the managed target

## Source

https://github.com/Apuyuseng/houttuynia (`nas` branch)

## License

Same as the source repository.
