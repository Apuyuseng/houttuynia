// mw_qemu 自测 ← crates/mwine-qemu 各模块 #[cfg(test)] + tests/integration.rs（mock 部分）
// 覆盖：端口分配（成对/跳占位）、argv 组装（AVD/镜像直载/datadir/snapshot/extra 覆盖）、
// 镜像目录校验（缺文件/残缺 ramdisk）、QemuRuntime dyld 路径、console token/KO 判定、
// closed port 优雅失败、detached 句柄行为、MockLauncher、DirectLauncher 前置校验。
// 不拉起 qemu 进程（真机集成测试对应对应 Rust --ignored 用例，不在自测内）。
#include "qemu.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QTcpServer>
#include <QTemporaryDir>

#include <cstdio>

using namespace mw::qemu;

static int g_failures = 0;
static int g_total = 0;

#define CHECK(cond)                                                                  \
    do {                                                                             \
        ++g_total;                                                                   \
        if (!(cond)) {                                                               \
            ++g_failures;                                                            \
            qWarning().noquote() << QStringLiteral("FAIL %1:%2 %3")                   \
                                       .arg(QString::fromLatin1(__FILE__),            \
                                            QString::number(__LINE__),                \
                                            QString::fromLatin1(#cond));              \
        }                                                                            \
    } while (false)

static QemuConfig base()
{
    QemuConfig c;
    c.avdName = QStringLiteral("MWine1");
    return c;
}

static void testPickPortPairAdjacent()
{
    const auto p = pickPortPair(5554);
    CHECK(p.has_value());
    CHECK(p->second == p->first + 1);
    CHECK(p->first >= 5554);
    // 选出的端口对当下确实空闲
    CHECK(portFree(p->first));
}

static void testPickPortPairSkipsOccupied()
{
    // 占住探测起点，迫使分配器跳到下一对
    QTcpServer hold;
    CHECK(hold.listen(QHostAddress::LocalHost, 0));
    const quint16 held = hold.serverPort();
    const quint16 start = held - (held % 2); // 对齐到偶数，保证 held 恰落在候选 console 上
    const auto p = pickPortPair(start);
    CHECK(p.has_value());
    CHECK(p->first != start);
    CHECK(p->first > start);
}

static void testPickPortSingle()
{
    const auto p = pickPort(8554);
    CHECK(p.has_value());
    CHECK(*p >= 8554);
}

static void testArgsAvdModeCoversBlueprintSurface()
{
    const QStringList args = base().buildArgs(5554, 5555, 8554);
    const QString joined = args.join(QChar(u' '));
    CHECK(joined.contains(QStringLiteral("-avd MWine1")));
    CHECK(joined.contains(QStringLiteral("-no-window")));
    CHECK(joined.contains(QStringLiteral("-no-audio")));
    CHECK(joined.contains(QStringLiteral("-memory 1536")));
    CHECK(joined.contains(QStringLiteral("-cores 4")));
    CHECK(joined.contains(QStringLiteral("-no-snapshot")));
    CHECK(joined.contains(QStringLiteral("-ports 5554,5555")));
    CHECK(joined.contains(QStringLiteral("-grpc 8554")));
    CHECK(joined.contains(QStringLiteral("-no-boot-anim")));
    // dataDir 为空：不传 -datadir，qemu 落回 AVD 自身目录
    CHECK(!args.contains(QStringLiteral("-datadir")));
}

static void testArgsExplicitDataDirIsPassed()
{
    QemuConfig cfg = base();
    cfg.dataDir = QStringLiteral("/data/mwine");
    const QString joined = cfg.buildArgs(5554, 5555, 8554).join(QChar(u' '));
    CHECK(joined.contains(QStringLiteral("-datadir /data/mwine")));
}

static void testArgsSnapshotAndExtraOverrideTail()
{
    QemuConfig cfg = base();
    cfg.snapshot = QStringLiteral("quick1");
    cfg.extraArgs = { QStringLiteral("-http-proxy"), QStringLiteral("http://127.0.0.1:7890") };
    const QStringList args = cfg.buildArgs(5556, 5557, 8556);
    CHECK(args.contains(QStringLiteral("-snapshot")));
    CHECK(args.contains(QStringLiteral("quick1")));
    CHECK(!args.contains(QStringLiteral("-no-snapshot")));
    // extra_args 拼在最后，可覆盖前面的默认值
    CHECK(args.size() >= 2);
    CHECK(args.at(args.size() - 2) == QStringLiteral("-http-proxy"));
    CHECK(args.last() == QStringLiteral("http://127.0.0.1:7890"));
}

static void testArgsImageDirModeUsesExplicitPaths()
{
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    QemuConfig cfg = base();
    cfg.imageDir = tmp.path();
    const QStringList args = cfg.buildArgs(5554, 5555, 8554);
    const QString joined = args.join(QChar(u' '));
    const QString img = QDir(tmp.path()).path();
    CHECK(joined.contains(QStringLiteral("-sysdir ") + img));
    CHECK(joined.contains(QStringLiteral("-system ") + QDir(img).filePath(QStringLiteral("system-qemu.img"))));
    CHECK(joined.contains(QStringLiteral("-ramdisk ") + QDir(img).filePath(QStringLiteral("ramdisk-qemu.img"))));
    CHECK(joined.contains(QStringLiteral("-kernel ") + QDir(img).filePath(QStringLiteral("kernel-ranchu"))));
    CHECK(!joined.contains(QStringLiteral("-vendor"))); // vendor 缺失时应省略而非报错
    CHECK(joined.contains(QStringLiteral("-id MWine1"))); // 保留身份标识便于 console 查询
}

static void testValidateImageDirRules()
{
    QTemporaryDir tmp;
    CHECK(tmp.isValid());
    const QDir d(tmp.path());

    // 空目录：缺 system-qemu.img
    {
        QemuConfig cfg = base();
        cfg.imageDir = tmp.path();
        const auto r = cfg.validateImageDir();
        CHECK(!r);
        CHECK(r.error().contains(QStringLiteral("system-qemu.img")));
    }
    // 文件齐全但 ramdisk 只有 1KB（构建不完整特征）
    {
        for (const char* f : { "system-qemu.img", "vendor-qemu.img", "kernel-ranchu", "build.prop" }) {
            QFile out(d.filePath(QString::fromLatin1(f)));
            CHECK(out.open(QIODevice::WriteOnly));
            out.write("x");
        }
        QFile ramdisk(d.filePath(QStringLiteral("ramdisk-qemu.img")));
        CHECK(ramdisk.open(QIODevice::WriteOnly));
        ramdisk.write(QByteArray(1024, '\0'));
        ramdisk.close();

        QemuConfig cfg = base();
        cfg.imageDir = tmp.path();
        const auto r = cfg.validateImageDir();
        CHECK(!r);
        CHECK(r.error().contains(QStringLiteral("1KB")));
    }
    // ramdisk 补足 9MB → 通过；imageDir 为空 → 恒通过
    {
        QFile ramdisk(d.filePath(QStringLiteral("ramdisk-qemu.img")));
        CHECK(ramdisk.open(QIODevice::WriteOnly));
        ramdisk.write(QByteArray(9 * 1024 * 1024, 'r'));
        ramdisk.close();
        QemuConfig cfg = base();
        cfg.imageDir = tmp.path();
        CHECK(cfg.validateImageDir());
        QemuConfig noImg;
        noImg.avdName = QStringLiteral("AVDOnly");
        CHECK(noImg.validateImageDir());
    }
}

static void testRuntimeDyldPathIncludesGlSubdirs()
{
    const QemuRuntime rt = QemuRuntime::bundle(QStringLiteral("/app/qemu"), QStringLiteral("/app"),
                                               QStringLiteral("/app/lib64"));
    CHECK(rt.dyldLibraryPath()
          == QStringLiteral("/app/lib64/vulkan:/app/lib64/gles_angle:/app/lib64"));
    CHECK(rt.validate().isErr()); // 二进制不存在
    CHECK(rt.validate().error().contains(QStringLiteral("qemu")));

    // SDK 探测在无 SDK 的环境下应安全返回空（或找到 SDK——两者都不算失败）
    const auto sdk = QemuRuntime::detectFromSdk();
    if (sdk.has_value())
        CHECK(QFileInfo(sdk->qemuBin).isFile());
    else
        CHECK(true);
}

static void testConsoleTokenAndKoDetection()
{
    const QString t = consoleToken();
    CHECK(!t.contains(QLatin1Char('\n'))); // token 不应含换行
    CHECK(isKo(QByteArrayLiteral("KO: unknown command")));
    CHECK(!isKo(QByteArrayLiteral("OK\r\n")));
}

static void testConsoleCommandOnClosedPortFailsGracefully()
{
    // 保留端口段（macOS 上 1 无监听且拒绝连接）
    const auto r = consoleCommand(1, QString(), QStringLiteral("avd name"));
    CHECK(!r);
    CHECK(r.error().contains(QStringLiteral("console")));
}

static void testDetachedHandleShape()
{
    const QemuHandle h = QemuHandle::detached(5556, 8556);
    CHECK(h.adbSerial() == QStringLiteral("emulator-5556"));
    CHECK(h.consolePort() == 5556);
    CHECK(h.grpcPort() == 8556);
    CHECK(h.pid() == 0);
    CHECK(!h.isRunning()); // detached 句柄无进程
    CHECK(h.shutdown());   // 无进程句柄关停应立即成功
    CHECK(h.sendConsole(QStringLiteral("kill")).isErr()); // 端口无监听 → 优雅失败
}

static void testMockLauncher()
{
    // 预设端口
    {
        MockLauncher m = MockLauncher::withPorts(5558, 8558);
        QemuConfig cfg = base();
        cfg.avdName = QStringLiteral("T");
        auto h = m.launch(cfg);
        CHECK(h);
        CHECK(h.value().consolePort() == 5558);
        CHECK(h.value().grpcPort() == 8558);
        CHECK(h.value().adbSerial() == QStringLiteral("emulator-5558"));
        CHECK(!h.value().isRunning()); // mock 句柄无进程
    }
    // 捕获配置 + 拒绝空目标
    {
        MockLauncher m;
        CHECK(m.launch(QemuConfig()).isErr()); // Error::NoTarget
        QemuConfig cfg = base();
        cfg.memoryMb = 2048;
        CHECK(m.launch(cfg));
        const auto captured = m.capturedConfig();
        CHECK(captured.has_value());
        CHECK(captured->avdName == QStringLiteral("MWine1"));
        CHECK(captured->memoryMb == 2048);
        CHECK(m.consoleResponder(QStringLiteral("x")) == QByteArrayLiteral("OK\r\n"));
    }
    // 集成测试形态（Rust tests/integration.rs mock 用例）
    {
        MockLauncher m = MockLauncher::withPorts(5560, 8560);
        QemuConfig cfg = base();
        cfg.avdName = QStringLiteral("IT");
        auto h = m.launch(cfg);
        CHECK(h);
        CHECK(h.value().adbSerial() == QStringLiteral("emulator-5560"));
        CHECK(!h.value().isRunning());
        CHECK(h.value().shutdown());
    }
}

static void testDirectLauncherPreflight()
{
    const QemuRuntime missing = QemuRuntime::bundle(QStringLiteral("/nonexistent/qemu"),
                                                    QStringLiteral("/nonexistent"),
                                                    QStringLiteral("/nonexistent/lib64"));
    const DirectLauncher l(missing);
    // 空目标 → NoTarget
    CHECK(!l.launch(QemuConfig()));
    // 有目标但二进制缺失 → BinaryNotFound
    QemuConfig cfg = base();
    const auto r = l.launch(cfg);
    CHECK(!r);
    CHECK(r.error().contains(QStringLiteral("qemu 二进制不存在")));
    // detect()：无 SDK 环境下应给出可读错误
    const auto det = DirectLauncher::detect();
    if (!det)
        CHECK(det.error().contains(QStringLiteral("qemu")));
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("mw_qemu_test"));

    testPickPortPairAdjacent();
    testPickPortPairSkipsOccupied();
    testPickPortSingle();
    testArgsAvdModeCoversBlueprintSurface();
    testArgsExplicitDataDirIsPassed();
    testArgsSnapshotAndExtraOverrideTail();
    testArgsImageDirModeUsesExplicitPaths();
    testValidateImageDirRules();
    testRuntimeDyldPathIncludesGlSubdirs();
    testConsoleTokenAndKoDetection();
    testConsoleCommandOnClosedPortFailsGracefully();
    testDetachedHandleShape();
    testMockLauncher();
    testDirectLauncherPreflight();

    qInfo().noquote() << QStringLiteral("mw_qemu_test: %1/%2 通过").arg(g_total - g_failures).arg(g_total);
    return g_failures == 0 ? 0 : 1;
}
