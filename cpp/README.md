# mwine C++ 工作区（mwine-cored / mwine CLI）

mwine 的 C++ 实现（当前唯一实现），**按业务模块拆分**：守护进程 `mwine-cored`
（REST 端口 7788，或 MWINE_PORT 环境变量）+ `mwine` CLI。UI 侧见 `app/qt/`
（Qt 前端自动拉起 `mwine-cored`）。原 Rust workspace（crates/）已从仓库删除，
下方模块对照表保留作溯源。

## 布局

```
cpp/
├── common/            基础库（mw::Result / HTTP 客户端·服务端 / gzip·哈希 / 子进程 / 原子写）
├── modules/           业务模块（静态库，与 Rust crate 一一对应）
│   ├── adb/           ← mwine-adb        smart-socket/wire/shell v1+v2/sync 协议客户端
│   ├── config/        ← mwine-config     VmConfig 持久化（37 字段，含校验）
│   ├── keymap/        ← mwine-keymap     按键映射数据模型 + v1 迁移 + 内置模板
│   ├── app/           ← mwine-app        APK 解析（zip+AXML）/ 安装 / .app 壳生成
│   ├── store/         ← mwine-store      豌豆荚 / F-Droid 商店，下载续传校验
│   ├── supervisor/    ← mwine-supervisor VM 生命周期 / 镜像导入拉取（zstd+tar）/ 快照
│   ├── capture/       ← mwine-capture    eBPF 流量采集：HTTP 明文重组 + sqlite 落库 + pcap TLS 解密
│   ├── qemu/          ← mwine-qemu       qemu 进程直管（fork+execve）/ console / 端口分配
│   ├── mitm/          ← mwine-mitm       HTTPS 中间人 CA（OpenSSL，与 Rust PEM 互通）
│   ├── scrcpy/        ← mwine-scrcpy     协议层全量 + VideoToolbox 硬解
│   ├── audio/         ← mwine-audio      CoreAudio 输出 + scrcpy 音频（flac→ffmpeg→PCM）
│   ├── input/         ← mwine-input+engine CGEventTap 捕获 / console·scrcpy 注入 / 映射引擎
│   ├── window/        （新）独立应用窗口播放器 mwine-window：scrcpy 虚拟显示器 +
│   │                    VideoToolbox 解码 + QOpenGLWidget NV12 渲染 + 控制注入 + 剪贴板互通
│   └── cli/           ← mwine-cli        mwine 命令行（业务能力全部来自 mw_* 模块；产物 build/modules/cli/mwine）
└── daemon/            ← mwine-core       mwine-cored：REST 路由 + 终端会话 + 商店 API
```

依赖：Qt 6（Core/Network，仅基础库直接依赖）、zstd、sqlite3、系统 zlib、OpenSSL（mitm/capture）。
模块只依赖 `mw_common` 与彼此的头文件（supervisor → adb）。

## 构建

```bash
cmake -B cpp/build -DCMAKE_PREFIX_PATH=$(brew --prefix qtbase) -DCMAKE_BUILD_TYPE=Release
cmake --build cpp/build -j            # 全部模块 + 守护进程
ctest --test-dir cpp/build            # 14 组模块测试（adb/mock server/tar/zstd/HPACK/出码配对…）
cpp/build/daemon/mwine-cored          # 守护进程（端口 7788，或 MWINE_PORT 环境变量）
cpp/build/modules/cli/mwine           # 命令行（开发树运行 Qt 时按此路径查找）
```

运行时行为：日志追加 `~/.mwine/logs/daemon.log`（>4MB 轮转）、孤儿 qemu
看护（清陈旧 AVD 锁、回收 adb 不可达超 3 分钟的实例）。

## 转换状态（约 2.7 万行 Rust → C++，已全量集成）

| 模块 | Rust 行数 | 测试 |
|---|---|---|
| adb | 1945 | 10 项（协议/同步/安装全链路，mock server） |
| config + keymap | 2669 | 全过（含 Rust 校验怪癖断言） |
| app | 2536 | 22 组（zip/AXML/壳生成，全离线；ADB 通道走 mw_adb 协议栈） |
| store | 1637 | 全过（三页面解析/镜像/续传/取消；含 impl_ 空指针修复） |
| supervisor | 1824 | 13 组（tar GNU 长名/pax/base-256、qemu 参数） |
| capture | 4413+1146 | 325 项（HPACK/H2/分块/sqlite/pcap TLS 1.2+1.3 真实握手解密/黄金样本） |
| daemon 路由 | 3939 | 30+ 端点真机实测（终端回环/截图/perf） |
| qemu + mitm | 1556 | 81 + 66（端口/console/CA 链校验，PEM 与 Rust 互通） |
| scrcpy + audio + input+engine | 9618 | 全过（22 种消息金样、VideoToolbox 硬解回环、FLAC→PCM 端到端、引擎假时钟全调度） |
| cli（mwine） | 7069 | 18 顶级命令全覆盖，真机回归（doctor/apps/files/keys/capture） |

工作区全量构建 0 错误，ctest **14/14 套件通过**。守护 `mwine-cored` 与 CLI `mwine` 均真机验证。

## 已知差距（后续迭代）

- capture 的 WebView pcap 设备模式真机回归未做（单测覆盖纯逻辑面；`webview-export` 已接真实现）
- daemon 的 bundle 迁移与通知桥已移植（见 main.cpp），打包态 .app 首启场景未真机验证
- `mwine debug displayfps/snap`、`debug displays` 列表仍是桩（display-id 副屏枚举依赖的
  emu console 查询未移植）；`debug setclip` 已实现（scrcpy 控制通道，写入+读回确认）
- `runtime/camera-image` 的 gRPC 主通道缺位，恒走 console 回退
- `keys watch` 已接线（mw_input 引擎：CGEventTap 捕获 → keymap 引擎 → console 内核注入，
  含辅助功能授权等待与前台门），多点触控/宏/FPS 捕获的长时间真机回归待做
- scrcpy 解码器仅 H.264（VideoToolbox H.265 参数集变体未做；server 端已协商为 h264）
- 独立窗口模式已在真机验证（虚拟显示器创建 / 视频流 / 录制），多点触控与 IME 深度场景待回归

## 模型约定（模块作者必读）

- `mw::Result<T>`：`Ok(v)/Err(msg)/ok()/value()/error()`，错误统一 QString
- 网络：`mw::httpGet/httpPost/httpRequest/httpDownload`（线程本地 QNAM，任意线程同步用）
- 进程：`mw::runProcess(program, args, timeoutMs)`
- 文件：`mw::readFile/writeFile`（原子写）、`mw::mwineDir()`
- 命名：Rust snake_case → camelCase；Duration → int ms；Vec<u8> → QByteArray
