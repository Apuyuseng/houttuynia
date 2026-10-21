# 鱼腥草 NAS 版部署指南

> `nas` 分支：把「在 Mac 上运行 Android 应用」变成**家庭服务器上的 Web 服务**——
> Rust 网关 `mwine-nas`（axum + 自研 adb/scrcpy 协议栈，无 Qt 依赖）对外开放
> REST API 并内嵌 Vue 3 + Element Plus 控制台——局域网内任何设备（手机 / 平板 /
> 电脑浏览器）都能管理虚拟机：开关机、看实时画面（scrcpy 投屏 + 触控）、
> 装应用、管镜像、开终端、改设置。

```
┌──────────────┐        浏览器 (http://nas-ip:7788)         ┌───────────────────────────┐
│ 手机/平板/电脑 │ ───────────────────────────────────────▶ │ NAS / 常开服务器            │
└──────────────┘                                          │  └─ mwine-nas（Rust）      │
                                                           │      ├─ /        Vue 控制台 │
                                                           │      ├─ /api/*   REST API   │
                                                           │      └─ Android 模拟器      │
                                                           └───────────────────────────┘
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
| 虚拟机开关机（快照存档）/ 状态 / 性能监控 | ✅ |
| scrcpy 实时投屏（WebCodecs 硬解）+ 触控/按键/滚轮/文本注入 | ✅ |
| 应用列表 / 启动 / 卸载 / APK 上传安装（进度）/ 真实图标（APK 提取） | ✅ |
| 截图 PNG 直出 / 截图下载 / 设备壁纸（root 镜像直读，桥应用导出兜底） | ✅ |
| ADB 交互终端（v2 pty 优先，v1 回落） | ✅ |
| 配置读写（与桌面版同 ~/.mwine/config.json）/ 快照管理 | ✅ |
| 镜像列表 / 切换 / 删除；真机列表（只读） | ✅ |
| 在线商店 / 镜像在线拉取 / 运行时下载 / 流量分析 / 真机配对 | ⏳ 501（用桌面端） |

---

## 一、三种部署形态

| 形态 | 适用 | VM 性能 | 说明 |
|---|---|---|---|
| **A. Docker（x86_64 NAS）** | 群晖 / 威联通 / 绿联 / TrueNAS 等 | ⚠️ ARM64 镜像靠指令翻译，慢，仅适合挂机类场景 | 最常见 NAS，开箱即用 |
| **B. Docker（ARM64 主机）** | ARM NAS / 树莓派 / RK3588 / ARM 服务器 | ❗ Google 未发布 linux-arm64 模拟器，需自行提供 emulator | Web 管理完整可用 |
| **C. Mac 服务器模式** | 常开的 Mac mini / Mac Studio | ✅ 全速（HVF 硬件加速，与桌面版一致） | **当前体验最好的形态** |

> **架构说明**：本产品的系统镜像是 ARM64 Android。x86_64 宿主运行 ARM64
> guest 只能走 qemu TCG 纯软件翻译（无硬件加速），交互操作很卡，适合
> 无头挂机（后台任务、定时脚本、抓包等）。要完整流畅体验请用形态 C。

---

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
  -p 7788:7788 \
  -e MWINE_TOKEN='改成强密码' \
  -v houttuynia-data:/data \
  --device /dev/kvm:/dev/kvm \
  houttuynia-nas
```

### 3. 安装模拟器运行时（首次必做）

镜像里不含 Android 模拟器（许可与体积原因），首次运行安装到持久卷：

```bash
docker exec -it houttuynia-nas /opt/mwine/setup-emulator.sh
```

装完在 compose 里取消 `MWINE_SDK_ROOT: /data/android-sdk` 注释并重启容器。
然后打开 Web 控制台「镜像」页拉取系统镜像即可。

### 4. 群晖 / 威联通图形化操作

- **群晖**：Container Manager → 项目 → 用 `nas/docker-compose.yml` 建项目；
  或 SSH 进 NAS 执行上面的命令。`/dev/kvm` 需要较新 x86 机型 + 套件内
  Virtual Machine Manager 已启用虚拟化；无 KVM 把 `devices:` 段删掉也能跑（更慢）。
- **威联通**：Container Station → 创建应用程序 → 粘贴 compose 内容。
- **绿联 / 极空间 / TrueNAS**：同理，都是标准 compose。

### 5. 打开控制台

浏览器访问 `http://<NAS的IP>:7788`，输入 `MWINE_TOKEN` 即可。

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
- `MWINE_TOKEN`：访问令牌；**对外开放时不设令牌 = 局域网任何人都能操控虚拟机**（启动时会大字警告）
- `MWINE_HEADLESS=1`：模拟器不弹窗口（`-no-window`），屏幕由 Web 投屏/截图通道承担
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
| `MWINE_PORT` | `7788` | 监听端口 |
| `MWINE_TOKEN` | 空 | 访问令牌；设置后所有 `/api/*` 需 `Authorization: Bearer <token>`（`<img>` 场景可用 `?token=`）。`/health` 恒开放供容器探活 |
| `MWINE_WEB_DIR` | 自动探测 | Web 控制台静态文件目录（需含 index.html） |
| `MWINE_HEADLESS` | 关 | 置位后模拟器以 `-no-window` 启动；Linux 上无 DISPLAY 时自动生效 |
| `MWINE_SDK_ROOT` | 自动探测 | 指定 Android SDK（运行时）根目录 |
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
2. 不要把 7788 端口直接映射到公网。需要远程访问请走 Tailscale /
   WireGuard / 反向代理 + HTTPS + 额外鉴权。
3. 上传安装接口只接受 zip 魔数的 APK，≤512MB，落盘在 `~/.mwine/uploads`
   （24h 自动清理）。
4. 静态文件服务有路径穿越防护（`..` 拒绝 + 符号链接落点校验），
   图标接口限定两个已知图标目录。

## 七、常见问题

- **打开页面提示「无法连接服务器」**：确认容器/守护进程在跑（`docker logs
  houttuynia-nas`），端口映射正确。
- **启动虚拟机报「SDK 探测失败」**：还没装运行时 → 跑 `setup-emulator.sh`
  并设置 `MWINE_SDK_ROOT`（形态 C 用本机 Android SDK 即可）。
- **x86_64 NAS 上虚拟机特别慢**：预期内（ARM64 镜像软件翻译）。确认
  `/dev/kvm` 已映射能略有改善；交互场景请换形态 C。
- **linux/arm64 主机无法启动 VM**：Google 没有 linux-arm64 模拟器官方构建，
  需要自行编译 emulator 放进 `$MWINE_SDK_ROOT/emulator/`。其余功能
  （应用库/商店/终端在 VM 运行后）不受影响。
- **图标全是字母块**：无头构建不含 iconfetch 子进程（避免 Qt Gui 依赖），
  应用图标显示为占位字母块，功能不受影响。
