// mw::supervisor ← crates/mwine-supervisor（lib.rs + cloud.rs）
// Android SDK / AVD / emulator 实例的生命周期管理 + 镜像导入/官方拉取 + 快照管理。
//
// 约定（Rust → C++）：
//   Result<T>            → mw::Result<T>（错误 QString；mw::Result<QString> 禁用，
//                          字符串值一律 mw::Result<QByteArray> 或出参）
//   Duration             → int 毫秒
//   Option<T>            → std::optional<T>（QString 可空用 std::optional<QString>）
//   PathBuf              → QString
//   BTreeMap<String,String> → QMap<QString,QString>
//
// adb 依赖：本头文件仅前置声明 mw::adb::AdbClient（按指针传递）；
// 实现侧通过 __has_include("adb.hpp") 自适应——头文件未就位时
// 依赖 adb 的函数体返回「待 adb 模块接入」错误（见 adb_avail.hpp）。
#pragma once

#include <QByteArray>
#include <QMap>
#include <QPair>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <QVector>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>

#include "result.hpp"

class QProcess;

namespace mw::adb {
class AdbClient; // 契约：值类型构造（127.0.0.1:5037）；devices()/shellBlocking()/findOnlineDevice()
}

namespace mw::supervisor {

class EmulatorHandle;
struct Sdk;

// ===================== Sdk：SDK 探测 / AVD 主目录 =====================

/// 一个 AVD 的描述信息（来自 <avd>.ini 与 <avd>.avd/config.ini）。
struct AvdInfo {
    QString name;
    QString path;                   // <avd>.avd 实体目录
    QMap<QString, QString> config;  // config.ini 解析结果

    QString abi() const;            // 空 = 未配置
    bool isArm64() const;
};

/// 探测到的 Android SDK（Rust: Sdk { root } 的展开形式）。
/// root 为 SDK 根；四个路径字段由 root 派生，daemon 可直接使用。
struct Sdk {
    QString emulatorBin;    // <root>/emulator/emulator
    QString adbBin;         // <root>/platform-tools/adb
    QString platformTools;  // <root>/platform-tools
    QString systemImages;   // <root>/system-images
    QString root;           // SDK 根（env 注入 / PATH 前置 / avdHome 判定用）

    static Sdk fromRoot(const QString& root);

    QString avdHome() const;
    bool isBundled() const;
    void seedAvdIfNeeded(const QString& name) const;
    QString findBuildTool(const QString& name) const;   // 空 = 未找到

    /// 列出本机全部 AVD（双 home：主 home + ~/.android/avd）。
    QVector<AvdInfo> listAvds() const;
    mw::Result<AvdInfo> findAvd(const QString& name) const;

    /// 外置运行时目录（~/Library/Application Support/MWine/runtime）。
    static QString externalRuntimeDir();
    /// 探测优先级：MWINE_SDK_ROOT > 外置运行时 > .app 内嵌运行时 >
    /// ANDROID_HOME/ANDROID_SDK_ROOT > ~/Library/Android/sdk。
    /// （ANDROID_* 不得高于内嵌运行时：装过 Android Studio 的 shell 普遍导出它们，
    ///   终端 CLI 会因此改用 SDK 版模拟器，与内嵌版并发打开同一 AVD 互踩锁。）
    static mw::Result<Sdk> detect();
private:
    static mw::Result<Sdk> detectUncached();
public:
};

/// 解析 Java properties 格式文本。
QMap<QString, QString> parseProperties(const QString& text);

// ===================== 设备模式 / 屏幕配置 =====================

/// 设备模式：决定屏幕尺寸 / 密度 / 设备名（手机 or 平板形态）。
enum class DeviceMode {
    Phone,   ///< 手机：1080x2400 @440dpi（约 392dp 宽，手机布局）
    Tablet,  ///< 平板：2560x1600 @320dpi 横屏（约 800dp 宽，smallestWidth≥600 触发平板 UI）
};

struct LcdSpec {
    int w = 0;
    int h = 0;
    int dpi = 0;
};

LcdSpec lcdSpec(DeviceMode mode);
const char* deviceName(DeviceMode mode);  // hw.device.name（mwine_phone / mwine_tablet）

/// 把 AVD 屏幕配置改写为任意自定义尺寸（config.ini 的 hw.lcd.* +
/// hardware-qemu.ini 同步覆盖，否则旧 hardware-qemu.ini 会压住新配置）。
mw::Result<bool> setLcd(const AvdInfo& avd, int w, int h, int dpi);
/// 把 AVD 屏幕配置改写为指定设备模式（hw.lcd.* 与 hw.device.name）。
mw::Result<bool> applyDeviceMode(const AvdInfo& avd, DeviceMode mode);
/// 运行时应用设备模式（guest 侧 wm size/density 覆盖；依赖 adb）。
mw::Result<bool> applyDeviceModeRuntime(mw::adb::AdbClient* adb, const QString& serial,
                                        DeviceMode mode);
/// 运行时设置任意尺寸/密度（wm size/density，立即生效；依赖 adb）。
mw::Result<bool> setLcdRuntime(mw::adb::AdbClient* adb, const QString& serial,
                               int w, int h, int dpi);

/// 本机虚拟 WiFi 名：首用生成（ChinaNet-xxxx / CMCC-xxxx），持久化
/// ~/.mwine/wifi-ssid，此后恒定（SSID 变了 guest 连不回已保存的网络）。
QString ensureWifiSsid();

// ===================== emulator 启动 =====================

/// emulator 启动选项（Rust: LaunchOptions）。
struct LaunchOptions {
    std::optional<QString> avdHome;   ///< AVD 主目录覆盖（child 的 ANDROID_AVD_HOME）
    QStringList extraArgs;            ///< 附加的原始 emulator 参数（如 -memory / -cores）
    bool noSnapshot = false;          ///< 冷启动（禁用快照加载/保存）
    std::optional<QString> snapshot;  ///< 启动时加载的快照名；空且非冷启时自动探测
    std::optional<QString> sdcard;    ///< 自定义 sdcard 镜像路径（空 = AVD 默认）
    bool noBootAnim = false;          ///< 跳过启动动画
    std::optional<DeviceMode> deviceMode;    ///< 设备模式（改写 AVD 屏幕尺寸与密度）
    std::optional<QString> imageDir;  ///< 自建镜像目录（设置后忽略 AVD 名，-sysdir 直接加载）
    std::optional<QString> proxy;     ///< HTTP 代理（guest 全部流量经宿主代理）
    std::optional<QString> dataDir;   ///< 用户数据目录（持久路径，升级不影响应用数据）
    std::optional<quint32> memoryMb;  ///< guest 内存 MB（qemu 层 -m 直传，绕过 2GB 下限钳制）
    bool windowed = false;            ///< 有窗口模式（默认 headless）
    /// 启动即有窗但隐藏主窗（-qt-hide-window；仅 windowed 时有意义）。
    /// window_mode_all 的"等效无头"：qemu -no-window 会把副屏帧按主屏尺寸
    /// 渲染（36.5.11 实测），故用官方隐藏选项而非真无头
    bool hideWindow = false;
    /// headless 时附加 -no-audio（默认 true，纯无头用法静音省开销）；
    /// window_mode_all 的无头仍要跑独立窗口应用 → false 保留 guest 音频
    bool noAudio = true;
    std::optional<QString> wifiSsid;  ///< 虚拟 WiFi SSID（必须每次启动同名）
};

/// 纯函数：拼装 emulator 参数（不含 env）。快照取值：opts.snapshot（显式指定）
/// 优先，否则 autoSnapshot（launch 侧经 latestSnapshot 自动探测的值）。
/// imageDir 模式忽略快照。顺序契约：emulator 自身参数必须全部位于 -qemu 透传块之前。
QStringList buildEmuArgs(const QString& avdName, const LaunchOptions& opts,
                         const std::optional<QString>& autoSnapshot);

/// 纯函数：拼装子进程环境（ANDROID_SDK_ROOT / ANDROID_ADB_SERVER_PORT /
/// PATH 前置 platform-tools / 零依赖模式的 AVD home 注入，avdHome 显式覆盖优先）。
QProcessEnvironment buildEmuEnv(const Sdk& sdk, const LaunchOptions& opts, int adbServerPort);

/// 自建镜像目录校验：关键文件齐全 + ramdisk 完整性（≥3MB 下界，
/// 构建不完整的 ramdisk 缺 init，内核会 panic 循环）。
mw::Result<bool> validateImageDir(const QString& imageDir);

/// 运行中的 emulator 实例（持有子进程）。
class EmulatorHandle {
public:
    EmulatorHandle() = default;
    ~EmulatorHandle();
    EmulatorHandle(EmulatorHandle&& o) noexcept;
    EmulatorHandle& operator=(EmulatorHandle&& o) noexcept;
    EmulatorHandle(const EmulatorHandle&) = delete;
    EmulatorHandle& operator=(const EmulatorHandle&) = delete;

    const QString& avd() const { return avd_; }
    const QString& serial() const { return serial_; }
    qint64 childId() const;          ///< emulator 子进程 pid（守护进程跟踪存活用）
    bool isRunning() const;

    /// headless 启动一个 AVD 或自建镜像目录，并等待其出现在 adb 设备列表。
    static mw::Result<EmulatorHandle> launchHeadless(const Sdk& sdk, const QString& avdName,
                                                     const LaunchOptions& opts,
                                                     mw::adb::AdbClient* adb);
    /// 等待系统完成引导（sys.boot_completed=1 且 bootanim 停止）。
    mw::Result<bool> waitBootCompleted(mw::adb::AdbClient* adb, int timeoutMs) const;
    /// 优雅关停（emulator 控制台 kill），超时（20s）后强杀进程。
    mw::Result<bool> shutdown(mw::adb::AdbClient* adb);

private:
    QString avd_;
    QString serial_;
    std::unique_ptr<QProcess> child_;
};

/// 探测 AVD 已保存的快照名：优先产品基准档 `mwine_default`，否则最新修改的一个。
std::optional<QString> latestSnapshot(const AvdInfo& avd);

// ===================== emulator 控制台（TCP） =====================

/// 从 serial（形如 emulator-5554）解析控制台端口。
mw::Result<int> consolePortFromSerial(const QString& serial);

/// 执行任意 emulator 控制台命令（与 `adb emu <cmd>` 同路径：控制台端口 + auth），
/// 返回控制台原始应答。
mw::Result<QByteArray> emuConsoleCmd(const QString& serial, const QString& cmd);
/// 同上，但读大缓冲（快照列表等长输出用；读到静默或 64KB 为止）。
mw::Result<QByteArray> emuConsoleCmdLarge(const QString& serial, const QString& cmd);
/// 通过 emulator 控制台协议优雅关停。
mw::Result<bool> emuConsoleKill(const QString& serial);

/// TCP adb 备用通道端口：guest 侧 adbd 监听 5555，宿主侧 console redir 用 5055
/// （避开 emulator 自占的宿主 5555）。
constexpr int kAdbTcpHostPort = 5055;
constexpr int kAdbTcpGuestPort = 5555;

/// 确保对该 emulator 实例存在可用的 adb 控制通道。管道健康则直接用 serial；
/// 失联时降级 console redir + TCP 连接，usableSerial 出参返回可用 serial。
mw::Result<bool> ensureControlChannel(mw::adb::AdbClient* adb, const QString& serial,
                                      QString* usableSerial);

/// 为应用创建独立直通显示器（multidisplay add 是 upsert 语义）。
mw::Result<bool> multidisplayAdd(const QString& serial, quint32 id, quint32 w, quint32 h,
                                 quint32 dpi);
/// 移除直通显示器（对应宿主窗口随之关闭）。
mw::Result<bool> multidisplayDel(const QString& serial, quint32 id);

/// 关停并等待 emulator 完全退出（优雅关停最长 timeoutMs；不等完就重启会端口冲突双杀）。
mw::Result<bool> emuKillWait(mw::adb::AdbClient* adb, const QString& serial, int timeoutMs);

// ===================== 快照存档点管理（console + 文件系统） =====================

/// 列出 AVD 目录下的快照存档点（含 mtime 毫秒；快照目录以 snapshot.pb 为完整标志）。
/// 新的在前。
QVector<QPair<QString, qint64>> avdSnapshotNames(const QString& avdDir);

/// 快照存档点保存（通用版）：支持新名字。保存是异步的：确认信号优先看
/// emulator 日志 "Saving snapshot '<name>' using N ms"，兜底看 snapshot.pb 出现/稳定。
mw::Result<bool> saveSnapshotAvd(const QString& serial, const QString& avdDir,
                                 const QString& avdName, const QString& snapName,
                                 int timeoutMs = 60000);
/// 关停前显式存档到指定档（emu kill 不会自动回存 -snapshot 指定的档）。
/// 兼容签名：按快照名在两台 AVD 主目录里反查所属 AVD，转调 saveSnapshotAvd。
mw::Result<bool> saveSnapshotBlocking(const QString& serial, const QString& name,
                                      int timeoutMs = 60000);
/// 运行中把 guest 回滚到指定存档点（console 热加载，guest 内部重启，宿主 qemu 不动）。
/// 加载期间 adb 短暂失联，轮询到 boot_completed 为止。
mw::Result<bool> loadSnapshotAvd(const QString& serial, const QString& avdDir,
                                 const QString& avdName, const QString& snapName,
                                 int timeoutMs = 60000);
/// 删除快照存档点：按 avdName 反查 AVD 目录，删 snapshots/<snapName>（文件系统侧）。
mw::Result<bool> removeSnapshot(const QString& avdName, const QString& snapName);

// ===================== 镜像导入 / 官方拉取（cloud.rs） =====================

using LogFn = std::function<void(const QString&)>;
using ProgressFn = std::function<void(const QString& phase, quint64 done, quint64 total)>;

/// 官方仓库可选的 system image（display 为 UI 展示名，如 "Android 13 · android-33;…"）。
struct OfficialImage {
    QString path;    ///< 形如 "android-33;default;arm64-v8a"；社区条目为 "community;<id>;arm64-v8a"
    quint64 size = 0;
    QString url;
    QString display;
    QString source;  ///< 空 = 官方仓库；"community" = 社区来源（GSI 等）
};

/// API 级别 → 用户可读的 Android 版本名（"android-33" → "Android 13"）。
std::optional<QString> androidVersionName(const QString& api);

/// 导入本地云分发包（tar.zst，内含 manifest.json 逐文件 sha256）。
/// installedDir 出参返回最终镜像目录。返回 bool（契约形态）。
mw::Result<bool> importImage(const QString& tarZstPath, const std::optional<QString>& sha256,
                             const QString& name, const Sdk& sdk, const LogFn& log,
                             const std::atomic_bool& cancel, const ProgressFn& prog,
                             QString* installedDir = nullptr);

/// 拉取官方仓库清单，列出可选 system image（仅 arm64-v8a；filter 为空 = 全部，
/// 可按 api 段或版本名过滤）。proxy 如 "http://127.0.0.1:7890"。
mw::Result<QVector<OfficialImage>> officialList(const QString& filter = QString(),
                                                const std::optional<QString>& proxy = std::nullopt);

/// 从 sys-img2-1.xml 解析 remotePackage（无 XML 依赖的轻量扫描；测试用）。
mw::Result<QVector<OfficialImage>> parseRepoXml(const QString& xml, const QString& filter);

/// 下载并安装一个官方 system image。target："android-33;default;arm64-v8a"
/// 或简写 "android-33"（取过滤后最新一项）。installedDir 出参返回安装目录。
mw::Result<bool> officialPull(const QString& target, const Sdk& sdk,
                              const std::optional<QString>& proxy, const LogFn& log,
                              const std::atomic_bool& cancel, const ProgressFn& prog,
                              QString* installedDir = nullptr);

/// 社区镜像目录（LineageOS GSI 等非 Google 来源；静态目录，不依赖官方仓库
/// 可达性，与 officialList 合并展示）。filter 匹配 display/id/版本名。
QVector<OfficialImage> communityList(const QString& filter = QString());

/// 下载社区 GSI 并以本地同 API 基础镜像（kernel/vendor/ramdisk 等）组装成
/// 完整 system-image 落地（GSI 只含 system 分区，基底提供其余部分）。
/// id 如 "lineage-gsi-23"。前提：本地已有同 API 的完整镜像（如 AOSP36）。
mw::Result<bool> communityPull(const QString& id, const Sdk& sdk,
                               const std::optional<QString>& proxy, const LogFn& log,
                               const std::atomic_bool& cancel, const ProgressFn& prog,
                               QString* installedDir = nullptr);

// ===================== 底层能力（tar/zstd/zip/哈希/稀疏化；测试与工具用） =====================

/// 增量 tar 流解析器：feed 任意分片（不必按 512 对齐），解包到 destDir。
/// 支持：常规文件 / 目录 / 符号链接 / 硬链接 / GNU 长名（'L'/'K'）/
/// pax path 覆盖（'x'）/ base-256 尺寸；拒绝路径穿越。
class TarStreamParser {
public:
    explicit TarStreamParser(QString destDir);
    ~TarStreamParser();
    TarStreamParser(const TarStreamParser&) = delete;
    TarStreamParser& operator=(const TarStreamParser&) = delete;

    mw::Result<bool> feed(const char* data, qint64 n);
    mw::Result<bool> finish();  ///< 流结束（校验不处于条目中间）

    const QStringList& entries() const;  // 已落地的相对路径

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};

/// 解压 tar.zst 到目录（流式 zstd + 自研 tar 解析；保留 symlink）。
mw::Result<bool> extractTarZst(const QString& tarZstPath, const QString& destDir);
/// 解压 zip 到目录（central directory 定位 + zlib raw inflate；路径防穿越）。
mw::Result<bool> zipExtract(const QString& zipPath, const QString& destDir);
/// 流式 sha256（4MiB 分块，逐块检取消点）。返回 hex 小写。
mw::Result<QByteArray> sha256File(const QString& path, const std::atomic_bool& cancel,
                                  const std::function<void(quint64)>& onBytes = nullptr);
/// 稀疏化：全零 4MiB 块跳过不写（APFS 打洞）。逻辑内容字节不变。
mw::Result<bool> sparseRewriteFile(const QString& path, const std::atomic_bool& cancel,
                                   const std::function<void(quint64)>& onBytes = nullptr);

// ===================== doctor：环境体检 =====================

struct DoctorItem {
    QString name;
    bool ok = false;
    QString detail;
};

QVector<DoctorItem> doctor();

} // namespace mw::supervisor
