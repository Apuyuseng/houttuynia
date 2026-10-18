// emulator 启动/关停 + 控制台 + 设备模式 ← lib.rs（LaunchOptions / EmulatorHandle /
// emu_console_* / set_lcd / apply_device_mode / ensure_wifi_ssid / emu_kill_wait）
#include "supervisor.hpp"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRandomGenerator>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>

#include "adb_avail.hpp"
#include "fs.hpp"

namespace mw::supervisor {

static QString homeDir()
{
    const QString h = mw::homeDir();
    return h.isEmpty() ? QStringLiteral("/Users/Shared") : h;
}

// ===================== 设备模式 =====================

LcdSpec lcdSpec(DeviceMode mode)
{
    switch (mode) {
    case DeviceMode::Phone:
        return { 1080, 2400, 440 };
    case DeviceMode::Tablet:
        return { 2560, 1600, 320 };
    }
    return { 1080, 2400, 440 };
}

const char* deviceName(DeviceMode mode)
{
    return mode == DeviceMode::Tablet ? "mwine_tablet" : "mwine_phone";
}

// config.ini / hardware-qemu.ini 的 lcd 键改写（在位替换；可选补缺、orientation、设备名）。
// emulator 运行时会写回 hardware-qemu.ini 且启动时优先生效——必须同步覆盖，
// 否则新 lcd 只改 config.ini 会被旧 hardware-qemu.ini 压住（表现为"设置不生效"）。
static mw::Result<QByteArray> rewriteLcdKeys(const QByteArray& text, int w, int h, int dpi,
                                             bool withOrientation, bool withDeviceName,
                                             const char* devName, bool appendMissing)
{
    QList<QByteArray> lines = text.split('\n');
    // 文本以换行结尾时，split 产生的末尾空元素会多补一个换行；对齐 Rust lines() 行为
    if (!lines.isEmpty() && lines.last().isEmpty())
        lines.removeLast();

    QByteArray out;
    out.reserve(text.size() + 128);
    QSet<QByteArray> seen;

    for (const QByteArray& raw : lines) {
        const QByteArray key = raw.split('=').value(0).trimmed();
        QByteArray repl;
        if (key == "hw.lcd.width")
            repl = "hw.lcd.width = " + QByteArray::number(w);
        else if (key == "hw.lcd.height")
            repl = "hw.lcd.height = " + QByteArray::number(h);
        else if (key == "hw.lcd.density")
            repl = "hw.lcd.density = " + QByteArray::number(dpi);
        else if (withOrientation && key == "hw.initialOrientation")
            repl = QByteArray("hw.initialOrientation = ") + (w > h ? "landscape" : "portrait");
        else if (withDeviceName && key == "hw.device.name")
            repl = QByteArray("hw.device.name = ") + devName;
        if (!repl.isEmpty()) {
            seen.insert(key);
            out += repl;
        } else {
            out += raw;
        }
        out += '\n';
    }
    if (appendMissing) {
        const QList<QPair<QByteArray, QByteArray>> missing = {
            { "hw.lcd.width", QByteArray::number(w) },
            { "hw.lcd.height", QByteArray::number(h) },
            { "hw.lcd.density", QByteArray::number(dpi) },
        };
        for (const auto& kv : missing) {
            if (!seen.contains(kv.first))
                out += kv.first + " = " + kv.second + '\n';
        }
        if (withOrientation && !seen.contains("hw.initialOrientation"))
            out += QByteArray("hw.initialOrientation = ") + (w > h ? "landscape" : "portrait") + '\n';
        if (withDeviceName && !seen.contains("hw.device.name"))
            out += QByteArray("hw.device.name = ") + devName + '\n';
    }
    return mw::Result<QByteArray>::Ok(out);
}

static mw::Result<bool> rewriteLcdFiles(const AvdInfo& avd, int w, int h, int dpi,
                                        bool withOrientation, bool withDeviceName,
                                        const char* devName, bool appendMissing)
{
    const QString cfg = QDir(avd.path).filePath(QStringLiteral("config.ini"));
    QFile f(cfg);
    if (!f.open(QIODevice::ReadOnly))
        return mw::err<bool>(QStringLiteral("读取 %1 失败: %2").arg(cfg, f.errorString()));
    const auto r1 = rewriteLcdKeys(f.readAll(), w, h, dpi, withOrientation, withDeviceName,
                                   devName, appendMissing);
    f.close();
    if (!r1)
        return mw::err<bool>(r1.error());
    if (auto werr = mw::writeFile(cfg, r1.value()); !werr)
        return mw::err<bool>(QStringLiteral("写入 %1 失败: %2").arg(cfg, werr.error()));

    // hardware-qemu.ini 同步（仅在位替换，不补缺——对齐 Rust 行为）
    const QString hwq = QDir(avd.path).filePath(QStringLiteral("hardware-qemu.ini"));
    if (QFileInfo::exists(hwq)) {
        QFile hf(hwq);
        if (hf.open(QIODevice::ReadOnly)) {
            const auto r2 = rewriteLcdKeys(hf.readAll(), w, h, dpi, false, false, nullptr, false);
            hf.close();
            if (r2)
                mw::writeFile(hwq, r2.value());
        }
    }
    return mw::Result<bool>::Ok();
}

mw::Result<bool> setLcd(const AvdInfo& avd, int w, int h, int dpi)
{
    return rewriteLcdFiles(avd, w, h, dpi, /*withOrientation=*/true, /*withDeviceName=*/false,
                           nullptr, /*appendMissing=*/true);
}

mw::Result<bool> applyDeviceMode(const AvdInfo& avd, DeviceMode mode)
{
    const LcdSpec lcd = lcdSpec(mode);
    return rewriteLcdFiles(avd, lcd.w, lcd.h, lcd.dpi, /*withOrientation=*/false,
                           /*withDeviceName=*/true, deviceName(mode), /*appendMissing=*/true);
}

mw::Result<bool> applyDeviceModeRuntime(mw::adb::AdbClient* adb, const QString& serial,
                                        DeviceMode mode)
{
#if MW_SUPERVISOR_ADB_READY
    const LcdSpec lcd = lcdSpec(mode);
    return setLcdRuntime(adb, serial, lcd.w, lcd.h, lcd.dpi);
#else
    Q_UNUSED(adb)
    Q_UNUSED(serial)
    Q_UNUSED(mode)
    return mw::err<bool>(QStringLiteral(kAdbPendingMsg));
#endif
}

mw::Result<bool> setLcdRuntime(mw::adb::AdbClient* adb, const QString& serial, int w, int h,
                               int dpi)
{
#if MW_SUPERVISOR_ADB_READY
    const QString cmd =
        QStringLiteral("wm size %1x%2 && wm density %3").arg(w).arg(h).arg(dpi);
    auto r = adb->shellBlocking(serial, cmd, 10000);
    if (!r)
        return mw::err<bool>(QStringLiteral("设置屏幕尺寸失败: %1").arg(r.error()));
    return mw::Result<bool>::Ok();
#else
    Q_UNUSED(adb)
    Q_UNUSED(serial)
    Q_UNUSED(w)
    Q_UNUSED(h)
    Q_UNUSED(dpi)
    return mw::err<bool>(QStringLiteral(kAdbPendingMsg));
#endif
}

// ===================== 虚拟 WiFi SSID =====================

QString ensureWifiSsid()
{
    const QDir home(homeDir());
    const QString p = home.filePath(QStringLiteral(".mwine/wifi-ssid"));
    QFile f(p);
    if (f.open(QIODevice::ReadOnly)) {
        const QString s = QString::fromUtf8(f.readAll()).trimmed();
        f.close();
        bool hasSpace = false;
        for (const QChar& c : s)
            hasSpace |= c.isSpace();
        if (s.size() >= 4 && s.size() <= 32 && !hasSpace)
            return s;
    }
    // 无依赖随机：系统 CSPRNG + 纳秒时间混合
    quint64 v = QRandomGenerator::system()->generate64();
    v ^= quint64(QDateTime::currentMSecsSinceEpoch()) * 0x9e3779b97f4a7c15ull;
    const char* prefix = (v & 1) ? "CMCC" : "ChinaNet";
    const QString ssid = QStringLiteral("%1-%2")
                             .arg(QString::fromLatin1(prefix),
                                  QString::number((v >> 8) & 0xffff, 16).rightJustified(4, QLatin1Char('0')));
    QDir(home).mkpath(QStringLiteral(".mwine"));
    QFile wf(p);
    if (wf.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        wf.write(ssid.toUtf8());
        wf.close();
    }
    return ssid;
}

// ===================== 参数 / 环境拼装（纯函数，可测） =====================

QStringList buildEmuArgs(const QString& avdName, const LaunchOptions& opts,
                         const std::optional<QString>& autoSnapshot)
{
    QStringList args;
    if (opts.imageDir) {
        // 自建镜像模式：-sysdir 直接加载，无需 AVD（ranchu 内核依赖 emulator
        // QEMU 分支的 goldfish 设备模型，启动器仍用 SDK emulator，仅替换镜像集）
        args << QStringLiteral("-sysdir") << *opts.imageDir
             << QStringLiteral("-system")
             << QDir(*opts.imageDir).filePath(QStringLiteral("system-qemu.img"));
        const QString vendor = QDir(*opts.imageDir).filePath(QStringLiteral("vendor-qemu.img"));
        if (QFileInfo::exists(vendor))
            args << QStringLiteral("-vendor") << vendor;
        args << QStringLiteral("-ramdisk")
             << QDir(*opts.imageDir).filePath(QStringLiteral("ramdisk-qemu.img"))
             << QStringLiteral("-kernel")
             << QDir(*opts.imageDir).filePath(QStringLiteral("kernel-ranchu"));
        // 固定控制台/adb 端口：serial 可预测（emulator-5556），自建镜像冷启慢，不做差分发现
    } else {
        // 快照：显式指定（opts.snapshot）> 自动探测最新快照（autoSnapshot，launch 侧解析）
        const std::optional<QString> snap =
            !opts.noSnapshot ? (opts.snapshot ? opts.snapshot : autoSnapshot) : std::nullopt;
        if (snap)
            args << QStringLiteral("-snapshot") << *snap;
        args << QStringLiteral("-avd") << avdName;
    }
    if (!opts.windowed) {
        args << QStringLiteral("-no-window");
        if (opts.noAudio)
            args << QStringLiteral("-no-audio");
    } else if (opts.hideWindow) {
        args << QStringLiteral("-qt-hide-window");
    }
    // GPU：不传 -gpu，走模拟器默认解析。教训：强制 swiftshader_indirect 会把
    // Vulkan ICD 切到弃用的 SwiftShader 后端（黑屏）。
    if (opts.noBootAnim)
        args << QStringLiteral("-no-boot-anim");
    if (opts.noSnapshot)
        args << QStringLiteral("-no-snapshot");
    if (opts.proxy)
        args << QStringLiteral("-http-proxy") << *opts.proxy;
    if (opts.dataDir) {
        QDir().mkpath(*opts.dataDir);
        args << QStringLiteral("-datadir") << *opts.dataDir;
    } else if (opts.imageDir) {
        args << QStringLiteral("-datadir") << QStringLiteral("/tmp/mwine-custom-data");
    }
    // emulator 自身的参数必须全部位于 -qemu 透传块之前
    // （-qemu 之后的参数原样交给 QEMU，emulator 不再解析）
    if (opts.sdcard)
        args << QStringLiteral("-sdcard") << *opts.sdcard;
    args << opts.extraArgs;
    // 虚拟 WiFi 名（-netsim-args 会把以 '-' 开头的后续参数当边界，
    // 故用 --wifi=SSID 等号单参形式；实测透传到 netsimd 进程参数）
    if (opts.wifiSsid)
        args << QStringLiteral("-netsim-args")
             << QStringLiteral("--wifi=") + *opts.wifiSsid;
    // 内存下探：qemu 层直传（emulator 会把 hw.ramSize 钳到镜像下限，绕开之）
    if (opts.memoryMb)
        args << QStringLiteral("-qemu") << QStringLiteral("-m")
             << QStringLiteral("%1M").arg(*opts.memoryMb);
    return args;
}

QProcessEnvironment buildEmuEnv(const Sdk& sdk, const LaunchOptions& opts, int adbServerPort)
{
    auto env = QProcessEnvironment::systemEnvironment();
    // console kill 后的优雅退出等待：默认 20s 是 vm stop 体感慢的大头
    //（快照已在 kill 前由 console save 同步落盘，无需长等待）
    env.insert(QStringLiteral("ANDROID_EMULATOR_WAIT_TIME_BEFORE_KILL"),
               QStringLiteral("3"));
    // emulator 需要 ANDROID_SDK_ROOT 解析系统路径（-sysdir 覆盖后依然校验）
    env.insert(QStringLiteral("ANDROID_SDK_ROOT"), sdk.root);
    // adb 服务器隔离：把端口传给模拟器。模拟器只向「默认 5037 或本 env 指定」的
    // 服务器注册自己——不传则产品私有端口上的 server 发现不了后启动的模拟器
    // （表现为 vm 启动超时 "emulator did not appear on adb"）。
    env.insert(QStringLiteral("ANDROID_ADB_SERVER_PORT"), QString::number(adbServerPort));
    // 模拟器会从 PATH 调 adb 干杂活（multidisplay 广播等）。把内嵌 platform-tools
    // 提到最前，确保与本产品 server 版本一致的 adb；否则用户 PATH 上的异版本 adb
    // 会对私有端口上的 server 触发「版本不符 kill-server」互杀。
    const QString path = env.value(QStringLiteral("PATH"));
    if (!path.isEmpty()) {
        QStringList dirs{ sdk.platformTools };
        dirs << path;
        env.insert(QStringLiteral("PATH"), dirs.join(QDir::listSeparator()));
    }
    // 零依赖模式：AVD home 一致性（种子在 launchHeadless 里做；此处保证子进程环境）
    if (sdk.isBundled()) {
        env.insert(QStringLiteral("ANDROID_AVD_HOME"), sdk.avdHome());
        // emulator 内部用 ANDROID_SDK_ROOT 找 platform-tools/adb
        env.insert(QStringLiteral("ANDROID_HOME"), sdk.root);
    }
    // AVD 实体在另一台主目录时显式覆盖（后设优先）
    if (opts.avdHome)
        env.insert(QStringLiteral("ANDROID_AVD_HOME"), *opts.avdHome);
    return env;
}

mw::Result<bool> validateImageDir(const QString& imageDir)
{
    const QDir dir(imageDir);
    for (const char* f : { "system-qemu.img", "ramdisk-qemu.img", "kernel-ranchu" }) {
        if (!QFileInfo::exists(dir.filePath(QString::fromLatin1(f))))
            return mw::err<bool>(QStringLiteral("镜像目录缺少 %1: %2")
                                     .arg(QString::fromLatin1(f), imageDir));
    }
    // ramdisk 完整性：曾出现构建机部分重编产物只有目录骨架、缺 init（内核 panic
    // "Unable to mount root fs"、adb 永久 offline）。合法 ramdisk 两种形态：boot+
    // vendor_ramdisk 拼接（~4.3MB）或旧式含模块全量（18MB+）。体积下界 3MB 兜底。
    QFileInfo ramdisk(dir.filePath(QStringLiteral("ramdisk-qemu.img")));
    if (!ramdisk.exists() || ramdisk.size() < 3ll * 1024 * 1024)
        return mw::err<bool>(
            QStringLiteral("ramdisk-qemu.img 仅 %1KB，构建不完整（缺 init，内核会 panic 循环）。"
                           "请在构建机完整重编后重新拉取镜像目录: %2")
                .arg(ramdisk.size() / 1024)
                .arg(imageDir));
    return mw::Result<bool>::Ok();
}

// ===================== EmulatorHandle =====================

EmulatorHandle::~EmulatorHandle() = default;
EmulatorHandle::EmulatorHandle(EmulatorHandle&& o) noexcept
    : avd_(std::move(o.avd_)), serial_(std::move(o.serial_)), child_(std::move(o.child_))
{
}
EmulatorHandle& EmulatorHandle::operator=(EmulatorHandle&& o) noexcept
{
    if (this != &o) {
        avd_ = std::move(o.avd_);
        serial_ = std::move(o.serial_);
        child_ = std::move(o.child_);
    }
    return *this;
}

qint64 EmulatorHandle::childId() const
{
    return child_ ? qint64(child_->processId()) : 0;
}

bool EmulatorHandle::isRunning() const
{
    return child_ && child_->state() == QProcess::Running;
}

mw::Result<EmulatorHandle> EmulatorHandle::launchHeadless(const Sdk& sdk, const QString& avdName,
                                                          const LaunchOptions& opts,
                                                          mw::adb::AdbClient* adb)
{
#if MW_SUPERVISOR_ADB_READY
    if (!QFileInfo::exists(sdk.emulatorBin) || !QFileInfo(sdk.emulatorBin).isFile())
        return mw::err<EmulatorHandle>(
            QStringLiteral("emulator binary not found: %1").arg(sdk.emulatorBin));

    // 通过 adb 设备列表差分发现新实例的 serial：先记录基线
    QStringList before;
    if (auto dr = adb->devices(); dr)
        for (const auto& d : dr.value())
            before << d.serial;

    std::optional<QString> resolvedSnapshot;
    if (opts.imageDir) {
        if (auto v = validateImageDir(*opts.imageDir); !v)
            return mw::err<EmulatorHandle>(v.error());
    } else {
        auto avdR = sdk.findAvd(avdName);
        if (!avdR)
            return mw::err<EmulatorHandle>(avdR.error());
        // 设备模式：改写屏幕尺寸/密度（手机/平板形态）
        if (opts.deviceMode) {
            if (auto r = applyDeviceMode(avdR.value(), *opts.deviceMode); !r)
                return mw::err<EmulatorHandle>(r.error());
        }
        if (!opts.noSnapshot && !opts.snapshot)
            resolvedSnapshot = latestSnapshot(avdR.value());
    }

    const QStringList args = buildEmuArgs(avdName, opts, resolvedSnapshot);
    // 零依赖模式首启：从 bundle 的 avd-template 种子出 AVD（幂等）
    if (sdk.isBundled() && !opts.imageDir)
        sdk.seedAvdIfNeeded(avdName);
    const auto env = buildEmuEnv(sdk, opts, adbServerPortOf(adb));

    // stdout/stderr 落盘（窗口与无头模式一致）：快照/渲染问题只打在这里，
    // 是远程排障弱机的唯一依据。轮转：超 8MB 截断（append 模式会无限增长）。
    std::optional<QString> logPath;
    {
        const QDir logDir(QDir(homeDir()).filePath(QStringLiteral(".mwine/logs")));
        if (logDir.mkpath(QStringLiteral("."))) {
            const QString log = logDir.filePath(QStringLiteral("emu-%1.log").arg(avdName));
            QFileInfo fi(log);
            if (fi.exists() && fi.size() > 8ll * 1024 * 1024)
                QFile::resize(log, 0);
            logPath = log;
        }
    }

    auto child = std::make_unique<QProcess>();
    child->setProgram(sdk.emulatorBin);
    child->setArguments(args);
    child->setProcessEnvironment(env);
    if (logPath) {
        child->setStandardOutputFile(*logPath, QIODevice::Append);
        child->setStandardErrorFile(*logPath, QIODevice::Append);
    } else {
        child->setStandardOutputFile(QProcess::nullDevice());
        child->setStandardErrorFile(QProcess::nullDevice());
    }
    child->start();
    if (!child->waitForStarted(5000)) {
        return mw::err<EmulatorHandle>(QStringLiteral("emulator launch failed: %1: %2")
                                           .arg(sdk.emulatorBin, child->errorString()));
    }

    // 通过 adb 设备列表差分发现新实例的 serial（自建镜像冷启慢，上限 120s）
    QElapsedTimer t0;
    t0.start();
    const qint64 kDeadlineMs = 120000;
    while (true) {
        if (auto dr = adb->devices(); dr) {
            for (const auto& d : dr.value()) {
                if (d.state == QStringLiteral("device") && !before.contains(d.serial)) {
                    EmulatorHandle h;
                    h.avd_ = avdName;
                    h.serial_ = d.serial;
                    h.child_ = std::move(child);
                    return mw::Result<EmulatorHandle>::Ok(std::move(h));
                }
            }
        }
        if (t0.elapsed() > kDeadlineMs)
            return mw::err<EmulatorHandle>(
                QStringLiteral("emulator did not appear on adb within timeout"));
        QThread::msleep(800);
    }
#else
    Q_UNUSED(sdk)
    Q_UNUSED(avdName)
    Q_UNUSED(opts)
    Q_UNUSED(adb)
    return mw::err<EmulatorHandle>(QStringLiteral(kAdbPendingMsg));
#endif
}

mw::Result<bool> EmulatorHandle::waitBootCompleted(mw::adb::AdbClient* adb, int timeoutMs) const
{
#if MW_SUPERVISOR_ADB_READY
    QElapsedTimer t0;
    t0.start();
    while (true) {
        if (auto r = adb->shellBlocking(serial_, QStringLiteral("getprop sys.boot_completed"), 5000);
            r && adbOutText(r.value()).trimmed() == QStringLiteral("1")) {
            const auto anim =
                adb->shellBlocking(serial_, QStringLiteral("getprop init.svc.bootanim"), 5000);
            const bool stopped =
                !anim || adbOutText(anim.value()).trimmed() == QStringLiteral("stopped");
            if (stopped)
                return mw::Result<bool>::Ok();
        }
        if (t0.elapsed() > timeoutMs)
            return mw::err<bool>(QStringLiteral("boot timeout (sys.boot_completed never reached 1)"));
        QThread::msleep(1000);
    }
#else
    Q_UNUSED(adb)
    Q_UNUSED(timeoutMs)
    return mw::err<bool>(QStringLiteral(kAdbPendingMsg));
#endif
}

mw::Result<bool> EmulatorHandle::shutdown(mw::adb::AdbClient* /*adb*/)
{
    // 优雅关停（emulator 控制台 kill），超时后强杀进程（Rust 侧 adb 参数未用）
    emuConsoleKill(serial_);
    QElapsedTimer t0;
    t0.start();
    while (t0.elapsed() < 20000) {
        if (!child_ || child_->state() != QProcess::Running)
            return mw::Result<bool>::Ok();
        QThread::msleep(500);
    }
    if (child_) {
        child_->kill();
        child_->waitForFinished(2000);
    }
    return mw::Result<bool>::Ok();
}

mw::Result<bool> emuKillWait(mw::adb::AdbClient* adb, const QString& serial, int timeoutMs)
{
#if MW_SUPERVISOR_ADB_READY
    if (auto k = emuConsoleKill(serial); !k)
        return mw::err<bool>(k.error());
    QElapsedTimer t0;
    t0.start();
    while (t0.elapsed() < qint64(timeoutMs)) {
        QThread::msleep(500);
        bool gone = true;
        if (auto dr = adb->devices(); dr) {
            gone = true;
            for (const auto& d : dr.value())
                if (d.serial == serial)
                    gone = false;
        }
        if (gone)
            return mw::Result<bool>::Ok();
    }
    return mw::err<bool>(QStringLiteral("%1 关停等待超时").arg(serial));
#else
    Q_UNUSED(adb)
    Q_UNUSED(serial)
    Q_UNUSED(timeoutMs)
    return mw::err<bool>(QStringLiteral(kAdbPendingMsg));
#endif
}

// ===================== emulator 控制台（TCP） =====================

mw::Result<int> consolePortFromSerial(const QString& serial)
{
    if (!serial.startsWith(QStringLiteral("emulator-")))
        return mw::err<int>(QStringLiteral("无法从 serial 解析控制台端口: %1").arg(serial));
    bool ok = false;
    const int port = serial.mid(QStringLiteral("emulator-").size()).toInt(&ok);
    if (!ok || port <= 0 || port > 65535)
        return mw::err<int>(QStringLiteral("无法从 serial 解析控制台端口: %1").arg(serial));
    return mw::Result<int>::Ok(port);
}

// 控制台协议：连 127.0.0.1:<port>，读问候 → auth token → 命令 → 应答
static mw::Result<QByteArray> emuConsoleCmdRaw(const QString& serial, const QString& cmd,
                                               bool large)
{
    auto portR = consolePortFromSerial(serial);
    if (!portR)
        return mw::err<QByteArray>(portR.error());

    QFile tf(QDir(homeDir()).filePath(QStringLiteral(".emulator_console_auth_token")));
    QString token;
    if (tf.open(QIODevice::ReadOnly)) {
        token = QString::fromUtf8(tf.readAll()).trimmed();
        tf.close();
    }

    QTcpSocket s;
    s.connectToHost(QStringLiteral("127.0.0.1"), quint16(portR.value()));
    if (!s.waitForConnected(3000))
        return mw::err<QByteArray>(QStringLiteral("连接 emulator 控制台失败: %1").arg(s.errorString()));
    // 读取控制台问候（读到静默或超时均可，控制台行为随版本略有差异）
    if (s.waitForReadyRead(3000))
        s.read(4096);
    if (!token.isEmpty()) {
        s.write(QStringLiteral("auth %1\n").arg(token).toUtf8());
        s.waitForBytesWritten(1000);
        if (s.waitForReadyRead(1500))
            s.read(256);
    }
    s.write(QStringLiteral("%1\n").arg(cmd).toUtf8());
    if (!s.waitForBytesWritten(1000))
        return mw::err<QByteArray>(QStringLiteral("发送控制台命令失败: %1").arg(s.errorString()));

    if (!large) {
        s.waitForReadyRead(3000);
        return mw::Result<QByteArray>::Ok(s.read(1024));
    }
    // 追读直到静默（单次 read 可能只拿到分片）；64KB 上限
    QByteArray out;
    while (s.waitForReadyRead(300)) {
        out += s.read(4096);
        if (out.size() > 64 * 1024)
            break;
    }
    return mw::Result<QByteArray>::Ok(out);
}

mw::Result<QByteArray> emuConsoleCmd(const QString& serial, const QString& cmd)
{
    return emuConsoleCmdRaw(serial, cmd, false);
}

mw::Result<QByteArray> emuConsoleCmdLarge(const QString& serial, const QString& cmd)
{
    return emuConsoleCmdRaw(serial, cmd, true);
}

mw::Result<bool> emuConsoleKill(const QString& serial)
{
    auto r = emuConsoleCmd(serial, QStringLiteral("kill"));
    if (!r)
        return mw::err<bool>(r.error());
    if (r.value().contains("KO"))
        return mw::err<bool>(
            QStringLiteral("emulator 控制台拒绝 kill: %1").arg(QString::fromUtf8(r.value()).trimmed()));
    return mw::Result<bool>::Ok();
}

mw::Result<bool> ensureControlChannel(mw::adb::AdbClient* adb, const QString& serial,
                                      QString* usableSerial)
{
#if MW_SUPERVISOR_ADB_READY
    // 管道健康则直接用，并幂等确保 guest 持久监听 TCP（防后续失联无解）
    if (auto r = adb->shellBlocking(serial, QStringLiteral("true"), 5000); r) {
        adb->shellBlocking(serial, QStringLiteral("setprop persist.adb.tcp.port 5555"), 5000);
        if (usableSerial)
            *usableSerial = serial;
        return mw::Result<bool>::Ok();
    }
    // 管道失联：console 加 redir（已存在时报错可忽略），adb server 连 TCP
    emuConsoleCmd(serial, QStringLiteral("redir add tcp:%1:%2")
                                 .arg(kAdbTcpHostPort)
                                 .arg(kAdbTcpGuestPort));
    const QString addr = QStringLiteral("localhost:%1").arg(kAdbTcpHostPort);
    auto cr = adb->connectNetwork(addr);
    if (!cr)
        return mw::err<bool>(QStringLiteral("TCP adb 通道建立失败（管道与 TCP 均不可用）: %1")
                                 .arg(cr.error()));
    const QString resp = adbOutText(cr.value());
    if (resp.contains(QStringLiteral("cannot connect"))
        || resp.contains(QStringLiteral("failed")))
        return mw::err<bool>(
            QStringLiteral("TCP adb 通道建立失败（管道与 TCP 均不可用）: %1").arg(resp));
    // 通道验证 + 幂等补设持久监听
    auto vr = adb->shellBlocking(addr, QStringLiteral("setprop persist.adb.tcp.port 5555"), 5000);
    if (!vr)
        return mw::err<bool>(QStringLiteral("TCP adb 通道不可用: %1").arg(vr.error()));
    if (usableSerial)
        *usableSerial = addr;
    return mw::Result<bool>::Ok();
#else
    Q_UNUSED(adb)
    Q_UNUSED(serial)
    Q_UNUSED(usableSerial)
    return mw::err<bool>(QStringLiteral(kAdbPendingMsg));
#endif
}

mw::Result<bool> multidisplayAdd(const QString& serial, quint32 id, quint32 w, quint32 h,
                                 quint32 dpi)
{
    auto r = emuConsoleCmd(serial, QStringLiteral("multidisplay add %1 %2 %3 %4 0")
                                          .arg(id)
                                          .arg(w)
                                          .arg(h)
                                          .arg(dpi));
    if (!r)
        return mw::err<bool>(r.error());
    if (r.value().contains("KO"))
        return mw::err<bool>(QStringLiteral("multidisplay add 被拒: %1")
                                 .arg(QString::fromUtf8(r.value()).trimmed()));
    return mw::Result<bool>::Ok();
}

mw::Result<bool> multidisplayDel(const QString& serial, quint32 id)
{
    auto r = emuConsoleCmd(serial, QStringLiteral("multidisplay del %1").arg(id));
    if (!r)
        return mw::err<bool>(r.error());
    if (r.value().contains("KO"))
        return mw::err<bool>(QStringLiteral("multidisplay del 被拒: %1")
                                 .arg(QString::fromUtf8(r.value()).trimmed()));
    return mw::Result<bool>::Ok();
}

} // namespace mw::supervisor
