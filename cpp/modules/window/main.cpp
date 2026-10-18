// mwine-window 入口：参数解析 + 会话引导。
// 由 `mwine run --window`（daemon 应用窗口模式）或 `mwine mirror` 拉起；
// 也可直接运行：mwine-window --serial emulator-5554 --package com.example.app
#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QSurfaceFormat>
#include <QWindow>

#include <cstdio>
#include <fcntl.h>
#include <unistd.h>

#include "chrome.hpp"
#include "window.hpp"

namespace {

struct DisplaySpec {
    quint32 w = 944, h = 2048, dpi = 384;
};

/// 解析 "WxH/DPI"、"WxH"、"/DPI"、""（与 NewDisplay::toParam 对偶）
DisplaySpec parseDisplay(const QString& s)
{
    DisplaySpec spec;
    QString sizePart = s;
    if (const int slash = s.indexOf(QLatin1Char('/')); slash >= 0) {
        const QString dpiPart = s.mid(slash + 1).trimmed();
        if (!dpiPart.isEmpty())
            spec.dpi = dpiPart.toUInt();
        sizePart = s.left(slash);
    }
    const int x = sizePart.indexOf(QLatin1Char('x'));
    if (x > 0) {
        const quint32 w = sizePart.left(x).toUInt();
        const quint32 h = sizePart.mid(x + 1).toUInt();
        if (w > 0 && h > 0) {
            spec.w = qMin(w, quint32(3840));
            spec.h = qMin(h, quint32(3840));
        }
    }
    return spec;
}

} // namespace

int main(int argc, char* argv[])
{
    // NV12 渲染走 3.3 core（QOpenGLTexture + GL_R8/GL_RG8）
    QSurfaceFormat fmt = QSurfaceFormat::defaultFormat();
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    QSurfaceFormat::setDefaultFormat(fmt);

    // 修饰键原生映射（⌘=Meta、⌃=Control）。Qt 默认在 macOS 互换两者：
    // 物理 ⌃ 会被报成 MetaModifier（被 ⌘ 快捷键块吞掉）、⌘ 被报成
    // ControlModifier（⌘W/⌘F 等全部失灵错位），必须关掉互换。
    QApplication::setAttribute(Qt::AA_MacDontSwapCtrlAndMeta);
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("mwine-window"));
    // QSettings（窗口几何记忆）需要 org 名
    app.setOrganizationName(QStringLiteral("Houttuynia"));
    app.setQuitOnLastWindowClosed(true);

    // 日志落盘：~/Library/Logs/mwine-window/<serial>.log（open 拉起无终端，
    // 排障只能靠文件；与 CLI 壳的 mwine-apps 日志同风格）
    {
        const QString logDir =
            QDir::home().filePath(QStringLiteral("Library/Logs/mwine-window"));
        QDir().mkpath(logDir);
        QString tag = QString::fromLocal8Bit(qgetenv("MWINE_LOG_TAG"));
        if (tag.isEmpty())
            tag = QStringLiteral("window");
        const int fd = ::open(
            QFile::encodeName(QDir(logDir).filePath(tag + QStringLiteral(".log"))).constData(),
            O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd >= 0) {
            ::dup2(fd, 1);
            ::dup2(fd, 2);
            std::fprintf(stderr, "\n==== %s pid=%ld ====\n",
                         QDateTime::currentDateTime().toString(Qt::ISODate).toUtf8().constData(),
                         static_cast<long>(QCoreApplication::applicationPid()));
        }
    }

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("mwine 独立应用窗口（scrcpy 虚拟显示器 + Qt 播放器）"));
    parser.addHelpOption();
    parser.addOptions({
        { { QStringLiteral("s"), QStringLiteral("serial") },
          QStringLiteral("目标设备 serial（缺省唯一在线设备）。"), QStringLiteral("serial") },
        { { QStringLiteral("p"), QStringLiteral("package") },
          QStringLiteral("窗口模式：会话建立后在虚拟显示器上启动该应用。"), QStringLiteral("pkg") },
        { { QStringLiteral("t"), QStringLiteral("title") },
          QStringLiteral("窗口标题。"), QStringLiteral("title") },
        { QStringLiteral("icon"),
          QStringLiteral("应用图标缓存 PNG（底部功能条左侧展示）。"), QStringLiteral("path") },
        { { QStringLiteral("d"), QStringLiteral("display") },
          QStringLiteral("虚拟显示器规格 WxH/DPI（默认 944x2048/384）。"), QStringLiteral("spec") },
        { QStringLiteral("mirror"), QStringLiteral("镜像模式：不建虚拟显示器，镜像主屏。") },
        { QStringLiteral("display-id"),
          QStringLiteral("镜像指定显示器（仅 --mirror）。"), QStringLiteral("id") },
        { QStringLiteral("record"),
          QStringLiteral("裸流 H.264 录制到文件（.h264）。"), QStringLiteral("file") },
        { QStringLiteral("no-audio"), QStringLiteral("禁用音频路由。") },
        { QStringLiteral("bitrate"),
          QStringLiteral("视频码率 bps（0 = 按设备默认：模拟器 12M / 真机 10M）。"),
          QStringLiteral("bps") },
        { QStringLiteral("max-size"),
          QStringLiteral("视频最长边像素上限（0 = 默认：模拟器原始 / 真机镜像 1600）。"),
          QStringLiteral("px") },
        { QStringLiteral("max-fps"),
          QStringLiteral("帧率上限（真机镜像默认 60；0 = 不限）。"), QStringLiteral("fps") },
        { QStringLiteral("borderless"),
          QStringLiteral("无边框窗口（顶栏空白处拖动移动；等价 MWINE_BORDERLESS=1，"
                         "open 拉 bundle 时不继承环境变量故需参数形态）。") },
    });
    parser.process(app);

    mwine::WindowOptions opt;
    opt.serial = parser.value(QStringLiteral("serial"));
    opt.package = parser.value(QStringLiteral("package"));
    opt.title = parser.value(QStringLiteral("title"));
    opt.iconPath = parser.value(QStringLiteral("icon"));
    opt.mirror = parser.isSet(QStringLiteral("mirror"));
    opt.recordPath = parser.value(QStringLiteral("record"));
    // 调试/取证：MWINE_RECORD 环境变量同样开启裸流录制（daemon spawn 场景无法传参）
    if (opt.recordPath.isEmpty()) {
        const QByteArray envRec = qgetenv("MWINE_RECORD");
        if (!envRec.isEmpty())
            opt.recordPath = QString::fromLocal8Bit(envRec);
    }
    opt.noAudio = parser.isSet(QStringLiteral("no-audio"));
    opt.borderless = parser.isSet(QStringLiteral("borderless"));
    opt.bitrate = parser.value(QStringLiteral("bitrate")).toUInt();
    opt.maxSize = parser.value(QStringLiteral("max-size")).toUInt();
    if (const auto f = parser.value(QStringLiteral("max-fps")).toFloat(); f > 0)
        opt.maxFps = f;
    if (parser.isSet(QStringLiteral("display-id")))
        opt.displayId = parser.value(QStringLiteral("display-id")).toUInt();
    const DisplaySpec spec = parseDisplay(parser.value(QStringLiteral("display")));
    opt.dispW = spec.w;
    opt.dispH = spec.h;
    opt.dispDpi = spec.dpi;

    // 缺省 serial：唯一在线设备（窗口通常由 CLI/daemon 指定后拉起，这里兜底）
    if (opt.serial.isEmpty()) {
        const auto dev = mw::adb::AdbClient().findOnlineDevice();
        if (!dev.ok()) {
            std::fprintf(stderr, "✗ 无在线设备：%s\n", qPrintable(dev.error()));
            return 1;
        }
        opt.serial = dev.value().serial;
    }

    auto* w = new mwine::HostWidget(std::move(opt));
    w->show();
    // 窗口外观：圆角 + 阴影（chrome.hpp 跨平台接口，各平台实现自适配；
    // MWINE_NO_ROUND=1 恢复直角对比）
    mwine::applyWindowChrome(w->windowHandle(),
                             qEnvironmentVariableIsSet("MWINE_NO_ROUND") ? 0.0 : 12.0);
    // ⌘Q/系统退出路径不走 closeEvent：aboutToQuit 里补一发 close，
    // 触发两段式收尾（停会话/副屏回收在 collector 线程，不拖住退出）
    QObject::connect(&app, &QCoreApplication::aboutToQuit, w, [w] { w->close(); });
    return app.exec();
}
