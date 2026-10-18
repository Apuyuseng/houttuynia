// scrcpy 4.0 server 启动参数（key=value 形式，经 app_process 传入）。
// 字段名与 server/src/main/java/.../Options.java 的解析逻辑一一对应。
// ← crates/mwine-scrcpy/src/options.rs
#pragma once

#include <QPair>
#include <QString>
#include <QVector>

#include <optional>

#include "audio.hpp"
#include "error.hpp"
#include "result.hpp"

namespace mw::scrcpy {

/// 与自带 server jar 版本严格一致（server 端校验版本号，不匹配即退出）。
/// v4.1 相对 4.0：线协议兼容（VP8/VP9 为纯增量），修复了 flex display 与
/// 编码器尺寸约束的多处回归。
inline const char* kServerVersion = "4.1";
inline const char* kServerClass = "com.genymobile.scrcpy.Server";
inline const char* kServerRemotePath = "/data/local/tmp/scrcpy-server";
inline const char* kSocketName = "scrcpy";

/// 每会话独立的远端 jar 路径：并发会话共用同一路径会在推送时互相覆盖，
/// 后启动会话把先启动会话正在加载的字节码写坏（表现为会话超时/黑屏窗口）。
inline QString sessionRemotePath(quint32 session)
{
    return QStringLiteral("%1.%2").arg(QString::fromLatin1(kServerRemotePath),
                                       QString::number(session, 16));
}

enum class VideoCodecId {
    H264,
    H265,
    Av1,
};

inline quint32 videoCodecWireId(VideoCodecId c)
{
    switch (c) {
    case VideoCodecId::H264: return 0x68323634; // "h264"
    case VideoCodecId::H265: return 0x68323635; // "h265"
    case VideoCodecId::Av1: return 0x00617631;  // "av1"
    }
    return 0;
}

inline mw::Result<VideoCodecId> videoCodecFromWire(quint32 id)
{
    switch (id) {
    case 0x68323634: return mw::Result<VideoCodecId>::Ok(VideoCodecId::H264);
    case 0x68323635: return mw::Result<VideoCodecId>::Ok(VideoCodecId::H265);
    case 0x00617631: return mw::Result<VideoCodecId>::Ok(VideoCodecId::Av1);
    default:
        return mw::err<VideoCodecId>(
            errProtocol(QStringLiteral("unknown video codec id: 0x%1").arg(id, 0, 16)));
    }
}

inline const char* videoCodecName(VideoCodecId c)
{
    switch (c) {
    case VideoCodecId::H264: return "h264";
    case VideoCodecId::H265: return "h265";
    case VideoCodecId::Av1: return "av1";
    }
    return "";
}

enum class LogLevel { Verbose, Debug, Info, Warn, Error };

inline const char* logLevelName(LogLevel l)
{
    switch (l) {
    case LogLevel::Verbose: return "verbose";
    case LogLevel::Debug: return "debug";
    case LogLevel::Info: return "info";
    case LogLevel::Warn: return "warn";
    case LogLevel::Error: return "error";
    }
    return "info";
}

/// 虚拟显示器尺寸：`(width, height, dpi)`。
/// `new_display` 参数支持 "WxH/DPI"、"WxH"、"/DPI"、"" 四种形式。
struct NewDisplay {
    std::optional<quint32> width;
    std::optional<quint32> height;
    std::optional<quint32> dpi;

    static NewDisplay make(quint32 w, quint32 h, quint32 dpi)
    {
        return NewDisplay{w, h, dpi};
    }

    QString toParam() const
    {
        QString size;
        if (width && height)
            size = QStringLiteral("%1x%2").arg(*width).arg(*height);
        if (dpi)
            return size + QStringLiteral("/%1").arg(*dpi);
        return size;
    }
};

struct ServerOptions {
    LogLevel logLevel = LogLevel::Info;
    bool video = true;
    bool audio = false;
    bool control = true;
    VideoCodecId videoCodec = VideoCodecId::H264;
    /// 音频编码：宿主侧裸流解码按 flac 设计（可直接喂 ffmpeg flac demuxer），
    /// opus 需 OGG 封装，暂不使用
    AudioCodecId audioCodec = AudioCodecId::Flac;
    quint32 videoBitRate = 8000000;
    quint32 maxSize = 0;
    std::optional<float> maxFps;
    /// 会话 id（31 位非负值）。空 = 用固定的 "scrcpy" socket 名；
    /// 有值时 socket 名为 "scrcpy_%08x"，多会话并存必备。
    std::optional<quint32> scid;
    /// 创建独立虚拟显示器（本项目"每应用一个窗口"的基石）
    std::optional<NewDisplay> newDisplay;
    /// 镜像既有显示器（与 newDisplay 互斥）。直通窗口的输入注入通道
    /// 用它绑定副屏：video 可以缩到微流仅满足 server 约束，控制消息
    /// （触摸/滚轮/键盘）按 socket 级延迟直达该显示器。
    std::optional<quint32> displayId;
    /// 4.0 flex display：虚拟显示器可动态调整尺寸
    bool flexDisplay = false;
    /// 关闭虚拟显示器时销毁其内容
    bool vdDestroyContent = true;
    /// 虚拟显示器启用系统装饰（状态栏等）
    bool vdSystemDecorations = true;
    bool stayAwake = true;
    bool powerOffOnClose = false;
    /// 会话空闲时不退出（对应用窗口保持很重要）
    bool keepActive = false;
    bool showTouches = false;

    /// 本会话使用的 abstract socket 名（与 server 端 DesktopConnection.getSocketName 一致）。
    QString socketName() const
    {
        if (!scid)
            return QString::fromLatin1(kSocketName);
        return QStringLiteral("%1_%2").arg(QString::fromLatin1(kSocketName),
                                           QString::number(*scid, 16).rightJustified(8, QLatin1Char('0')));
    }

    /// 生成一个随机 31 位 scid（时间 + pid + 地址熵混合）。
    static quint32 randomScid();

    /// 序列化为 app_process 参数（不含类名与版本号）。
    /// 注意：scid 出现两次（%08x 与 %x）为 Rust 实现的既定行为——
    /// server 端 Options 逐项覆盖解析，最终生效的是后者（十六进制）。
    QVector<QString> toArgs() const;

    /// 校验参数组合合法性（与 server 端约束一致）。
    mw::Result<bool> validate() const;
};

/// 组装完整的 app_process 命令行（CLASSPATH 指向已推送的 server jar）。
QString serverCommand(const QString& remotePath, const QVector<QString>& args);

/// 常见 scrcpy-server 安装位置。
/// 优先 mwine 自带版本（与客户端协议严格对齐；brew 的 jar 随上游漂移，
/// 版本不匹配时 server 立即退出 → 连接头读失败 "failed to fill whole buffer"）。
/// 未找到返回空串。
QString findServerJar();

} // namespace mw::scrcpy
