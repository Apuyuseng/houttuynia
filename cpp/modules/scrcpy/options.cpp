// options 实现 ← crates/mwine-scrcpy/src/options.rs
#include "options.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <chrono>
#include <cstdlib>
#include <unistd.h>

namespace mw::scrcpy {

quint32 ServerOptions::randomScid()
{
    using namespace std::chrono;
    const auto now = steady_clock::now().time_since_epoch();
    const quint64 nanos = quint64(duration_cast<nanoseconds>(now).count());
    const quint64 secs = quint64(duration_cast<seconds>(now).count());
    const quint64 timeMix = nanos ^ secs;
    const quint64 pid = quint64(::getpid());
    quint64 stack = 0;
    const quint64 stackAddr = reinterpret_cast<quint64>(&stack);
    const quint64 mixed = (timeMix * 0x9E3779B97F4A7C15ULL)
                          ^ ((pid << 32) | (pid >> 32))
                          ^ ((stackAddr << 17) | (stackAddr >> 47));
    return quint32(mixed & 0x7FFFFFFFu);
}

QVector<QString> ServerOptions::toArgs() const
{
    QVector<QString> args;
    auto push = [&args](const char* k, const QString& v) {
        args.append(QStringLiteral("%1=%2").arg(QString::fromLatin1(k), v));
    };
    if (scid) {
        // 与 socketName() 保持一致：server 端按 scid 绑定 scrcpy_<scid>
        push("scid", QString::number(*scid, 16).rightJustified(8, QLatin1Char('0')));
    }
    push("log_level", QString::fromLatin1(logLevelName(logLevel)));
    push("video", video ? QStringLiteral("true") : QStringLiteral("false"));
    push("audio", audio ? QStringLiteral("true") : QStringLiteral("false"));
    push("control", control ? QStringLiteral("true") : QStringLiteral("false"));
    push("video_codec", QString::fromLatin1(videoCodecName(videoCodec)));
    if (audio)
        push("audio_codec", QString::fromLatin1(audioCodecName(audioCodec)));
    push("video_bit_rate", QString::number(videoBitRate));
    push("max_size", QString::number(maxSize));
    if (scid) {
        // server 端按 16 进制解析（Options.parse: Integer.parseInt(value, 0x10)）
        // 与 Rust 实现一致：scid 参数重复出现，最终生效此处（无补零）的十六进制值
        push("scid", QString::number(*scid, 16));
    }
    if (maxFps) {
        // Rust format!("{fps}")：float 默认显示（10.0 → "10"）
        const double v = double(*maxFps);
        push("max_fps", QString::number(v, 'g', 10));
    }
    if (displayId)
        push("display_id", QString::number(*displayId));
    if (newDisplay) {
        push("new_display", newDisplay->toParam());
        push("vd_destroy_content", vdDestroyContent ? QStringLiteral("true") : QStringLiteral("false"));
        push("vd_system_decorations",
             vdSystemDecorations ? QStringLiteral("true") : QStringLiteral("false"));
        if (flexDisplay)
            push("flex_display", QStringLiteral("true"));
    }
    push("stay_awake", stayAwake ? QStringLiteral("true") : QStringLiteral("false"));
    push("power_off_on_close", powerOffOnClose ? QStringLiteral("true") : QStringLiteral("false"));
    push("keep_active", keepActive ? QStringLiteral("true") : QStringLiteral("false"));
    push("show_touches", showTouches ? QStringLiteral("true") : QStringLiteral("false"));
    // forward 模式：server 监听 abstract socket，宿主主动连
    push("tunnel_forward", QStringLiteral("true"));
    return args;
}

mw::Result<bool> ServerOptions::validate() const
{
    if (!video && !audio)
        return mw::err<bool>(errInvalidOption(QStringLiteral("video 与 audio 不能同时关闭")));
    if (displayId && newDisplay) {
        return mw::err<bool>(
            errInvalidOption(QStringLiteral("display_id 与 new_display 互斥（server 端 assert 约束）")));
    }
    if (displayId && *displayId > quint32(0x7FFFFFFF))
        return mw::err<bool>(errInvalidOption(QStringLiteral("display_id 越界: %1").arg(*displayId)));
    if (newDisplay && newDisplay->width && newDisplay->height) {
        if (*newDisplay->width == 0 || *newDisplay->height == 0)
            return mw::err<bool>(errInvalidOption(QStringLiteral("new_display 尺寸必须为正")));
    }
    if (flexDisplay && !newDisplay) {
        return mw::err<bool>(
            errInvalidOption(QStringLiteral("flex_display 需要 new_display（虚拟显示器）")));
    }
    return mw::Result<bool>::Ok();
}

QString serverCommand(const QString& remotePath, const QVector<QString>& args)
{
    QString cmd = QStringLiteral("CLASSPATH=%1 app_process / %2 %3")
                      .arg(remotePath, QString::fromLatin1(kServerClass),
                           QString::fromLatin1(kServerVersion));
    for (const QString& a : args) {
        cmd += QLatin1Char(' ');
        cmd += a;
    }
    return cmd;
}

QString findServerJar()
{
    const QByteArray home = qgetenv("HOME");
    const QString homeDir = home.isEmpty() ? QString::fromLatin1("/root") : QString::fromLocal8Bit(home);
    // bundle 内置位：可执行同目录的 ../Resources/bin（Houttuynia.app 与
    // 每应用窗口包装 .app 都是这个布局；干净机器没有 ~/.mwine/bin，此为关键兜底）
    const QStringList bundled = [] {
        QStringList out;
        const QString bin = QDir(QCoreApplication::applicationDirPath())
                                .filePath(QStringLiteral("../Resources/bin"));
        for (const char* v : { "scrcpy-server-v4.1", "scrcpy-server-v4.0" })
            out.append(QDir(bin).filePath(QString::fromLatin1(v)));
        return out;
    }();
    const QVector<QString> candidates = {
        homeDir + QStringLiteral("/.mwine/bin/scrcpy-server-v4.1"),
        homeDir + QStringLiteral("/.mwine/bin/scrcpy-server-v4.0"),
        bundled.at(0),
        bundled.at(1),
        QStringLiteral("/opt/homebrew/share/scrcpy/scrcpy-server"),
        QStringLiteral("/opt/homebrew/opt/scrcpy/share/scrcpy/scrcpy-server"),
        QStringLiteral("/usr/local/share/scrcpy/scrcpy-server"),
    };
    for (const QString& p : candidates) {
        if (QFileInfo(p).isFile())
            return p;
    }
    return QString();
}

} // namespace mw::scrcpy
