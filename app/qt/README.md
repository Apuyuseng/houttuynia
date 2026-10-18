# Houttuynia Qt 前端（跨平台：macOS / Linux / Windows）

与 C++ 守护（mwine-cored，127.0.0.1:7788）通过 HTTP/JSON 通信；CLI 集成（安装 /
卸载 / 按键映射）走同仓 `cpp/modules/cli` 产出的 `mwine` 命令。

## Qt 许可合规（LGPLv3，不购买商业版）

- 使用 Qt 官方开源版（Qt 6），**动态链接**（默认构建即动态；禁止静态链接进可执行文件）
- 不修改 Qt 本体（如有补丁必须开源该补丁）
- 应用内「关于」页声明使用 Qt 并附带 LGPLv3 文本（见 mainwindow.cpp showAbout）
- 分发包随附 Qt 动态库与 LGPLv3 许可文本；用户替换 Qt 库的能力天然保留（动态链接）
- 我们的业务代码可继续闭源（LGPL 不传染）

## 构建

### macOS（开发机已验证）
```bash
brew install qtbase cmake
cmake -B build -DCMAKE_PREFIX_PATH=$(brew --prefix qtbase) -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
# 部署为独立 .app（把 brew Qt 框架拷进包，动态链接保持）：
mkdir -p Houttuynia.app/Contents/MacOS && cp build/Houttuynia Houttuynia.app/Contents/MacOS/
macdeployqt Houttuynia.app
```

### Linux
```bash
# Debian/Ubuntu: apt install qt6-base-dev cmake ninja-build
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
# 分发：linuxdeployqt 或发行版包依赖 qt6-base（系统动态库即合规）
```

### Windows
```bash
# 安装 Qt 6 开源版（MSVC 或 MinGW）+ CMake，然后：
cmake -B build -DCMAKE_PREFIX_PATH="C:/Qt/6.x/msvc2019_64" && cmake --build build --config Release
windeployqt --release build/Houttuynia.exe   # DLL 随包，动态链接保持
```

## 状态与路线

- [x] v0.3.0-qt：**9 页全量对齐 SwiftUI 版**——概览（开关机/截屏/性能速览/快照概览/运行时横幅）、
      应用库（网格/双击启动/右键模式切换与卸载/APK 拖放安装/内置「在线下载」应用弹独立窗口：
      豌豆荚+F-Droid 双源、镜像选择、版本列表、下载进度+取消、自动安装+装完删包）、镜像（本地管理/官方在线拉取/任务进度）、
      ADB 终端（自动连接/历史↑↓/Ctrl-C/清屏）、性能（CPU/内存/宿主开销/3 分钟曲线）、
      按键映射（模板套用/方案管理/画面校准截图/映射开关）、流量分析（启停/筛选/明文详情）、
      设置（形态/性能/摄像头/网络/磁盘扩容/快照/危险区）；深浅色跟随系统；关于页含 LGPLv3 声明
- [x] C++ 守护栈（`cpp/`）已全量落地：adb / config / keymap / app / store / supervisor /
      capture / qemu / mitm / scrcpy / audio / input 按业务模块静态库 + `mwine-cored`
      守护进程 + `mwine` CLI（原 Rust workspace 已删除，详见仓库根 `cpp/README.md`）
- [x] 独立窗口播放器（`cpp/modules/window` → mwine-window）：scrcpy 虚拟显示器 +
      VideoToolbox 硬解 + QOpenGLWidget NV12 渲染 + 控制注入 + 剪贴板互通（真机已验）
- [ ] 后端跨平台化：emuhide AppKit 调用与 scrcpy 解码器 VideoToolbox（macOS-only）
      需换 FFmpeg/软解以支持 Win/Linux
