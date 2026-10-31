# 鱼腥草 NAS 版部署指南

> `nas` 分支：**纯真机 Android 网关**——Rust 网关 `mwine-nas`（axum + 自研
> adb/scrcpy 协议栈，无 Qt 依赖）对外开放 REST API 并内嵌 Vue 3 + Element Plus
> 控制台。局域网内任何设备（手机 / 平板 / 电脑浏览器）都能管理局域网 / USB
> 连接的 Android 设备（手机 / 电视 / 盒子）：投屏 + 触控、应用库（图标/安装/
> 卸载）、ADB 终端、截图与壁纸。**无内置虚拟机**（模拟器能力属于桌面版）。

```
┌──────────────┐        浏览器 (http://nas-ip:7788)         ┌───────────────────────────┐
│ 手机/平板/电脑 │ ────── HTTPS ──────▶ ┌─────────────────────────┐      ┌─ Android 设备（adb）─┐
└──────────────┘                        │ NAS / 常开服务器          │ ◀──▶ │ 手机 / TV / 盒子      │
                                        │  └─ mwine-nas（Rust）     │      └──────────────────────┘
                                        │      ├─ /        Vue 控制台│
                                        │      └─ /api/*   REST API  │
                                        └─────────────────────────┘
```

## 技术栈与双后端说明

- **后端**：`nas/rust/`（crate `mwine-nas`）——axum + tokio，自研 adb
  smart-socket 客户端（含 host:tport/transport_id 新协议）与 scrcpy v4.1
  协议栈（推 jar / 转发 / 会话 / 视频解复用 / 控制注入），零 Qt 依赖。
- **前端**：`web-vue/`（Vue 3 + Element Plus + Vite，深色主题），构建产物
  `web-vue/dist` 随源码分发，NAS 部署不需要 node。
- **桌面对照**：C++ 守护进程（`mwine-cored`）与本网关共用同一套 REST 契约，
  桌面 Qt 客户端能力更全（商店/抓包/图标等）；网关暂未实现的接口一律返回
  501 并在页面给出中文提示。

### 网关能力矩阵（Rust 版 v0.1）

| 能力 | 状态 |
|---|---|
| scrcpy 实时投屏（WebCodecs 硬解）+ 触控/按键/滚轮/文本注入 | ✅ |
| 设备性能监控（CPU/内存/运行时长） | ✅ |
| 宿主输入法（IME 组合文本注入，CJK 走剪贴板粘贴通道） | ✅ |
| 剪贴板双向共享（设备变化自动同步宿主；宿主粘贴/同步进设备） | ✅ |
| 应用独立窗口（new_display 副屏直通 + 浏览器浮窗，可拖/缩放/多开） | ✅ |
| 应用列表 / 启动 / 卸载 / APK 上传安装（进度）/ 真实图标（APK 提取） | ✅ |
| 截图 PNG 直出 / 截图下载 / 设备壁纸（root 镜像直读，桥应用导出兜底） | ✅ |
| ADB 交互终端（v2 pty 优先，v1 回落） | ✅ |
| 多设备管理（连接 / 断开 / 扫码配对 / 选定管理目标） | ✅ |
| 真机配对（扫码 / 配对码：SPAKE2 + TLS1.3 + AES-GCM 全自研实现）/ 连接 / 断开 | ✅ |
| 在线商店 | ⏳ 501（用桌面端） |

---

## 一、两种部署形态

| 形态 | 适用 | 说明 |
|---|---|---|
| **A. Docker** | 群晖 / 威联通 / 绿联 / TrueNAS / 任何常开服务器 | 最常见，开箱即用；amd64 与 arm64 均可 |
| **B. Mac 服务器模式** | 常开的 Mac mini / Mac Studio | 不需要 Docker，直接跑网关 |

> 架构说明：amd64 宿主可直接用官方镜像；arm64 宿主用 `nas/Dockerfile.prebuilt`
> （musl 静态二进制 + scratch，零网络拉取）。

## 二、形态 A/B：Docker 部署

### 1. 构建镜像

```bash
git clone <仓库> && cd mwine
git checkout nas
docker build -t houttuynia-nas -f nas/Dockerfile .
```

### 2. 启动（docker compose，推荐）

```bash
cd nas
# 先改 docker-compose.yml 里的 MWINE_TOKEN（这是 Web 控制台登录密码！）
docker compose up -d
```

或纯 docker：

```bash
docker run -d --name houttuynia-nas \
  --network host \
  -e MWINE_TOKEN='改成强密码' \
  -e MWINE_TLS_PORT=18443 \
  -v houttuynia-data:/data \
  houttuynia-nas
```

### 3. 群晖 / 威联通图形化操作

- **群晖**：Container Manager → 项目 → 用 `nas/docker-compose.yml` 建项目；
  或 SSH 进 NAS 执行上面的命令。
- **威联通**：Container Station → 创建应用程序 → 粘贴 compose 内容。
- **绿联 / 极空间 / TrueNAS**：同理，都是标准 compose。

### 5. 打开控制台

浏览器访问 `https://<NAS的IP>:18443`（自签证书，接受一次警告），输入 `MWINE_TOKEN` 即可。

---

## 三、形态 C：Mac 服务器模式（推荐）

不需要 Docker。在一台常开 Mac 上构建 Rust 网关：

```bash
# 1) 构建 Rust 网关（需 Rust 工具链）
cd nas/rust && cargo build --release && cd ../..

# 2) 以 NAS 模式启动（Web 控制台自动探测 web-vue/dist）
MWINE_HOST=0.0.0.0 \
MWINE_TOKEN='改成强密码' \
MWINE_HEADLESS=1 \
./nas/rust/target/release/mwine-nas
```

- `MWINE_HOST=0.0.0.0`：对局域网开放（默认只听 127.0.0.1）
- `MWINE_TOKEN`：访问令牌；**对外开放时不设令牌 = 局域网任何人都能操控连接的设备**（启动时会大字警告）
- `MWINE_WEB_DIR`：自定义前端目录；不设置时自动探测 `web-vue/dist` / `web/`
- `MWINE_PORT`：端口，默认 7788
- `MWINE_SDK_ROOT`：Android SDK 位置（默认自动探测本机 SDK / MWine 外置运行时）

想开机自启，把上面的命令包进 `launchd` plist（`KeepAlive=true`）。

> 桌面版 C++ 守护（mwine-cored）同样支持 NAS 模式（同一套环境变量 + REST 契约），
> 需要商店/抓包/图标等完整能力时可用它替代：构建见仓库根 README。

---

## 四、环境变量一览

| 变量 | 默认 | 说明 |
|---|---|---|
| `MWINE_HOST` | `127.0.0.1` | 绑定地址；`0.0.0.0` 对外开放 |
| `MWINE_TLS_PORT` | 空（HTTP 模式） | **TLS-only 监听端口**：置位后只起 HTTPS、完全不绑 HTTP（`MWINE_PORT` 被忽略）。自签证书自动生成为 `~/.mwine/tls/{cert,key}.pem`（重启不变，浏览器只需接受一次自签警告）。**对外部署一律用它**——WebCodecs 投屏只在 https 安全上下文可用 |
| `MWINE_PORT` | `7788` | HTTP 监听端口（仅在未设 `MWINE_TLS_PORT` 时生效；本机开发/调试用） |
| `MWINE_TOKEN` | 空 | 访问令牌；设置后所有 `/api/*` 需 `Authorization: Bearer <token>`（`<img>` 场景可用 `?token=`）。`/health` 恒开放供容器探活 |
| `MWINE_WEB_DIR` | 自动探测 | Web 控制台静态文件目录（需含 index.html） |
| `MWINE_REPLACE_DAEMON` | 关 | 置位后新守护会替换占端口的旧守护 |

## 五、Web 控制台新增的 API（相对桌面版）

| 路由 | 说明 |
|---|---|
| `GET /api/screenshot.png` | 设备屏幕 PNG 直出（`?serial=` 可选），截图/兜底画面用 |
| `GET /api/apps/icon?pkg=` | 应用图标 PNG（图标缓存库 + 安装任务图标目录） |
| `GET /api/wallpaper.png` | 设备壁纸 PNG |
| `POST /api/apps/install-upload?name=x.apk` | 原始字节上传 APK 并安装（`application/octet-stream`，≤512MB） |
| `POST /api/apps/<pkg>/uninstall` | 卸载应用（原 CLI 专属能力补齐到 REST） |
| `POST /api/stream/open` | 开启 scrcpy 投屏会话（`?max_size=&bit_rate=&fps=&serial=`），返回 `{id,codec,width,height}` |
| `GET /api/stream/<id>/video` | H.264 裸流长连接推送（自定义帧头：`MW`+type+pts+len，见 web/stream.js） |
| `POST /api/stream/<id>/touch` | 触摸注入 `{action:0/1/2, x, y}`（按下/抬起/移动，设备像素坐标） |
| `POST /api/stream/<id>/key` | 按键注入 `{keycode, action:tap/down/up}`（返回 4 / 主页 3 / 多任务 187…） |
| `POST /api/stream/<id>/text` | 文本注入 `{text}` |
| `POST /api/stream/<id>/scroll` | 滚轮 `{x, y, hs, vs}` |
| `POST /api/stream/<id>/reset` | 强制编码器重发 config+关键帧（解码错误自愈） |
| `POST /api/stream/<id>/close` | 关闭会话 |

其余 `/api/*` 与桌面版完全同契约（Qt 客户端与 Web 控制台共用）。

### 投屏实现说明（scrcpy over HTTP + WebCodecs）

- 守护进程复用自研 scrcpy v4.1 协议栈（`cpp/modules/scrcpy`）：起 server →
  端口转发 → 视频 socket 逐包解复用 → 自定义二进制帧头经分块 HTTP 长连接推给浏览器；
  浏览器侧 WebCodecs `VideoDecoder`（annexb H.264，硬件加速）解码渲染到 canvas。
- 触摸/滚轮/按键/文本经控制 socket 注入，线协议与桌面独立窗口逐字节一致。
- 画面 idle 无帧期间推流线程每秒探测浏览器连接活性，断开即回收会话；
  队列积压（慢网）自动触发 resetVideo 重对齐关键帧。
- 浏览器要求：Chrome/Edge 94+、Safari 16.4+（WebCodecs H.264 annexb）；
  不支持的浏览器自动回退「截图轮询」模式（约 1fps）。
- **已知限制**：自带 scrcpy-server v4.1 在 **Android 5.0.2（API 21）原生 AOSP
  镜像**上无法投屏——该镜像 SurfaceControl 虚拟显示器创建不被支持
  （`IllegalStateException`），且 server jar 缺一个 D8 合成类
  （`Size$$ExternalSyntheticLambda0`）导致约束回退路径崩溃。产品镜像
  （Android 11+ userdebug）不受影响（桌面独立窗口同栈日用）。
  投屏失败时守护进程会明确报「设备未产出视频流」，前端自动回退截图模式。

### 本次顺带修复的 adb 栈问题（桌面版同样受益）

- `host:transport:<serial>` 旧协议在 adb server 34+ 上会让后续所有 shell v2
  服务 FAIL closed → 全面改用 `host:tport:<serial>`（并消费其 8 字节
  transport_id），旧 server 自动回落 legacy；
- `shellStream`（scrcpy server 拉起通道）新增 v2→v1 回落（对齐
  shellBlocking 既有策略）；
- scrcpy server 远端 jar 路径固定加 `.jar` 后缀（API 21 的 ART 对非 .jar
  CLASSPATH 条目直接 SIGABRT），并改为稳定路径 + 同尺寸跳过推送
  （老镜像免去每次开流的冷 dexopt 数秒）；
- 首探 dummy 字节读取放宽到 12s，消除「探测先于 server 就绪 → 掐死刚
  accept 的 server」竞态。

## 六、安全须知

1. **必须设置 `MWINE_TOKEN`**。令牌等同 root 权限：ADB 终端可执行任意 guest
   命令，设置接口可改代理/参数。请用强随机串，并只在可信局域网开放。
2. 不要把 18443（TLS）端口直接暴露到公网。需要远程访问请走 Tailscale /
   WireGuard / 反向代理 + HTTPS + 额外鉴权。
3. 上传安装接口只接受 zip 魔数的 APK，≤512MB，落盘在 `~/.mwine/uploads`
   （24h 自动清理）。
4. 静态文件服务有路径穿越防护（`..` 拒绝 + 符号链接落点校验），
   图标接口限定两个已知图标目录。

## 七、常见问题

- **打开页面提示「无法连接服务器」**：确认容器/守护进程在跑（`docker logs
  houttuynia-nas`），端口正确。
- **应用列表为空 / 真机连不上**：先确认宿主机 adb server 在跑（`adb devices`
  能看到设备）；无线设备需要设备侧开「无线调试」。
- **投屏不可用**：必须走 `https://`（WebCodecs 只在安全上下文暴露），
  用内置 TLS 端口（默认部署 18443），首次访问接受一次自签证书警告。
- **图标全是字母块**：图标后台预热中（在线商店包名查询 + APK 提取），落盘后
  SSE 自动推新列表渐出；确认无图标的应用才保持字母块兜底。
- **真机（无线调试/扫码配对）连不上**：mDNS 组播不过 Docker 桥接网络 →
  compose 必须用 `network_mode: host`（本仓库 compose 已默认）。host 网络下
  端口直接落在宿主机，注意与宿主机已有端口冲突（adb server 共享 5037）。
- **多台设备在线时操作了错误设备**：「真机」页用「设为当前」选定管理目标。
