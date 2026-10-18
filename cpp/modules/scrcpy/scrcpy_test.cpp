// mw_scrcpy 自测：ControlMessage 字节级 roundtrip / device_msg 解析 /
// options 序列化 / 视频音频解复用 fixture / Annex-B 纯函数 / VT 解码回环。
// 注意：不用 <cassert>（Release 下 NDEBUG 会使 assert 失效），自定义 CHECK。
#include <QByteArray>
#include <QCoreApplication>
#include <QDebug>
#include <QString>
#include <QtGlobal>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <optional>
#include <thread>

#include "scrcpy/scrcpy.hpp"

static int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            qCritical().noquote() << QStringLiteral("FAIL %1:%2 %3")            \
                                         .arg(QString::fromLatin1(__FILE__),   \
                                              QString::number(__LINE__),        \
                                              QString::fromLatin1(#cond));      \
        }                                                                      \
    } while (0)

/// 字节级相等（QByteArray == QByteArray 语义即可，见错误打印单独包装）。
static bool eqBytes(const QByteArray& got, const QByteArray& want, const char* what)
{
    if (got == want)
        return true;
    ++g_failures;
    qCritical().noquote() << QStringLiteral("FAIL %1: got %2 want %3").arg(
        QString::fromLatin1(what), QString::fromLatin1(got.toHex()),
        QString::fromLatin1(want.toHex()));
    return false;
}

using namespace mw::scrcpy;
using namespace mw::scrcpy::control;

static QByteArray u64be(quint64 v)
{
    QByteArray b;
    for (int i = 7; i >= 0; --i)
        b.append(char(quint8(v >> (8 * i))));
    return b;
}
static QByteArray u32be(quint32 v)
{
    QByteArray b;
    for (int i = 3; i >= 0; --i)
        b.append(char(quint8(v >> (8 * i))));
    return b;
}
static QByteArray u16be(quint16 v)
{
    QByteArray b;
    b.append(char(quint8(v >> 8)));
    b.append(char(quint8(v)));
    return b;
}
static QByteArray i32be(qint32 v) { return u32be(quint32(v)); }

// ---- ControlMessage：全部 22 种类型的字节级金样 ----

static void testControlMessageEncode()
{
    // InjectKeycode：type action keycode repeat metaState
    {
        auto m = ControlMessage::injectKeycode(KeyAction::Down, 66, 1, 0x7000);
        auto b = m.encode();
        CHECK(b.ok());
        QByteArray want;
        want.append(char(kTypeInjectKeycode));
        want.append(char(0));
        want += i32be(66) + i32be(1) + i32be(0x7000);
        eqBytes(b.value(), want, "inject_keycode");
    }
    // InjectText：len u32 + utf8
    {
        auto m = ControlMessage::injectText(QStringLiteral("hi你"));
        auto b = m.encode();
        CHECK(b.ok());
        const QByteArray utf8 = QStringLiteral("hi你").toUtf8();
        QByteArray want;
        want.append(char(kTypeInjectText));
        want += u32be(quint32(utf8.size()));
        want += utf8;
        eqBytes(b.value(), want, "inject_text");
    }
    // 超长文本（> 300 字节）报协议错误
    {
        auto m = ControlMessage::injectText(QString(400, QLatin1Char('a')));
        auto b = m.encode();
        CHECK(b.isErr());
    }
    // InjectTouch：pressure 0.5 → 0x8000；buttons 按下
    {
        auto m = ControlMessage::injectTouch(ACTION_DOWN, kPointerIdMouse, 100, 200, 1280, 720,
                                             0.5f, 1, 1);
        auto b = m.encode();
        CHECK(b.ok());
        QByteArray want;
        want.append(char(kTypeInjectTouchEvent));
        want.append(char(ACTION_DOWN));
        want += u64be(kPointerIdMouse) + i32be(100) + i32be(200);
        want += u16be(1280) + u16be(720) + u16be(0x8000);
        want += i32be(1) + i32be(1);
        eqBytes(b.value(), want, "inject_touch");
    }
    // InjectScroll：h=-0.5 v=1.0 → i16 定点
    {
        auto m = ControlMessage::injectScroll(10, 20, 1920, 1080, -0.5f, 1.0f, 0);
        auto b = m.encode();
        CHECK(b.ok());
        QByteArray want;
        want.append(char(kTypeInjectScrollEvent));
        want += i32be(10) + i32be(20) + u16be(1920) + u16be(1080);
        want += u16be(quint16(qint16(-0.5f * 32767.0f)));
        want += u16be(quint16(qint16(1.0f * 32767.0f)));
        want += i32be(0);
        eqBytes(b.value(), want, "inject_scroll");
    }
    // BackOrScreenOn / 三个面板命令
    {
        auto b = ControlMessage::back(KeyAction::Up).encode();
        CHECK(b.ok());
        QByteArray want;
        want.append(char(kTypeBackOrScreenOn));
        want.append(char(1));
        eqBytes(b.value(), want, "back_or_screen_on");
        eqBytes(ControlMessage::expandNotificationPanel().encode().value(),
                QByteArray(1, char(kTypeExpandNotificationPanel)), "expand_notification");
        eqBytes(ControlMessage::expandSettingsPanel().encode().value(),
                QByteArray(1, char(kTypeExpandSettingsPanel)), "expand_settings");
        eqBytes(ControlMessage::collapsePanels().encode().value(),
                QByteArray(1, char(kTypeCollapsePanels)), "collapse_panels");
    }
    // GetClipboard
    {
        auto b = ControlMessage::getClipboard(COPY_KEY_CUT).encode();
        CHECK(b.ok());
        QByteArray want;
        want.append(char(kTypeGetClipboard));
        want.append(char(COPY_KEY_CUT));
        eqBytes(b.value(), want, "get_clipboard");
    }
    // SetClipboard：seq u64 + paste u8 + lp32 文本
    {
        auto m = ControlMessage::setClipboard(0x1122334455667788ULL, true, QStringLiteral("ok"));
        auto b = m.encode();
        CHECK(b.ok());
        const QByteArray utf8 = QByteArrayLiteral("ok");
        QByteArray want;
        want.append(char(kTypeSetClipboard));
        want += u64be(0x1122334455667788ULL);
        want.append(char(1));
        want += u32be(quint32(utf8.size()));
        want += utf8;
        eqBytes(b.value(), want, "set_clipboard");
    }
    // SetDisplayPower / RotateDevice
    {
        QByteArray want;
        want.append(char(kTypeSetDisplayPower));
        want.append(char(0));
        eqBytes(ControlMessage::setDisplayPower(false).encode().value(), want, "set_display_power");
        eqBytes(ControlMessage::rotateDevice().encode().value(),
                QByteArray(1, char(kTypeRotateDevice)), "rotate_device");
    }
    // UhidCreate：name 长度 1 字节 + data 长度 2 字节
    {
        auto m = ControlMessage::uhidCreate(3, 0x1234, 0x5678, QStringLiteral("mouse"),
                                            QByteArrayLiteral("\x01\x02"));
        auto b = m.encode();
        CHECK(b.ok());
        const QByteArray name8 = QByteArrayLiteral("mouse");
        QByteArray want;
        want.append(char(kTypeUhidCreate));
        want += u16be(3) + u16be(0x1234) + u16be(0x5678);
        want.append(char(uchar(name8.size())));
        want += name8;
        want += u16be(2);
        want += QByteArrayLiteral("\x01\x02");
        eqBytes(b.value(), want, "uhid_create");
    }
    // UhidInput / UhidDestroy
    {
        auto m = ControlMessage::uhidInput(7, QByteArrayLiteral("\xAB"));
        auto b = m.encode();
        CHECK(b.ok());
        QByteArray want;
        want.append(char(kTypeUhidInput));
        want += u16be(7) + u16be(1) + QByteArrayLiteral("\xAB");
        eqBytes(b.value(), want, "uhid_input");
        QByteArray want2;
        want2.append(char(kTypeUhidDestroy));
        want2 += u16be(7);
        eqBytes(ControlMessage::uhidDestroy(7).encode().value(), want2, "uhid_destroy");
    }
    // OpenHardKeyboardSettings / StartApp / ResetVideo / 相机 / ResizeDisplay
    {
        eqBytes(ControlMessage::openHardKeyboardSettings().encode().value(),
                QByteArray(1, char(kTypeOpenHardKeyboardSettings)), "open_hard_keyboard");
        const QByteArray name8 = QByteArrayLiteral("com.example.app");
        QByteArray wantApp;
        wantApp.append(char(kTypeStartApp));
        wantApp.append(char(uchar(name8.size())));
        wantApp += name8;
        eqBytes(ControlMessage::startApp(QStringLiteral("com.example.app")).encode().value(),
                wantApp, "start_app");
        eqBytes(ControlMessage::resetVideo().encode().value(),
                QByteArray(1, char(kTypeResetVideo)), "reset_video");
        QByteArray wantTorch;
        wantTorch.append(char(kTypeCameraSetTorch));
        wantTorch.append(char(1));
        eqBytes(ControlMessage::cameraSetTorch(true).encode().value(), wantTorch, "camera_torch");
        eqBytes(ControlMessage::cameraZoomIn().encode().value(),
                QByteArray(1, char(kTypeCameraZoomIn)), "camera_zoom_in");
        eqBytes(ControlMessage::cameraZoomOut().encode().value(),
                QByteArray(1, char(kTypeCameraZoomOut)), "camera_zoom_out");
        QByteArray wantResize;
        wantResize.append(char(kTypeResizeDisplay));
        wantResize += u16be(1024) + u16be(768);
        eqBytes(ControlMessage::resizeDisplay(1024, 768).encode().value(), wantResize,
                "resize_display");
    }
}

// ---- device_msg 解析 ----

static void testDeviceMessage()
{
    using mw::scrcpy::device_msg::kTypeClipboard;
    using mw::scrcpy::device_msg::kTypeAckClipboard;
    using mw::scrcpy::device_msg::kTypeUhidOutput;

    // Clipboard：type + lp32 文本
    {
        const QByteArray text = QStringLiteral("剪贴板").toUtf8();
        QByteArray wire;
        wire.append(char(kTypeClipboard));
        wire += u32be(quint32(text.size()));
        wire += text;
        BufferReader reader(wire);
        auto m = readDeviceMessage(reader);
        CHECK(m.ok());
        CHECK(m.value().has_value());
        CHECK(m.value()->kind == DeviceMessage::Kind::Clipboard);
        CHECK(m.value()->text == QStringLiteral("剪贴板"));
        // 流结束：Ok(nullopt)
        auto end = readDeviceMessage(reader);
        CHECK(end.ok());
        CHECK(!end.value().has_value());
    }
    // AckClipboard：sequence 回显
    {
        QByteArray wire;
        wire.append(char(kTypeAckClipboard));
        wire += u64be(42);
        BufferReader reader(wire);
        auto m = readDeviceMessage(reader);
        CHECK(m.ok());
        CHECK(m.value()->kind == DeviceMessage::Kind::AckClipboard);
        CHECK(m.value()->sequence == 42);
    }
    // UhidOutput：id + len + data
    {
        QByteArray wire;
        wire.append(char(kTypeUhidOutput));
        wire += u16be(9);
        wire += u16be(2);
        wire += QByteArrayLiteral("\x01\x02");
        BufferReader reader(wire);
        auto m = readDeviceMessage(reader);
        CHECK(m.ok());
        CHECK(m.value()->kind == DeviceMessage::Kind::UhidOutput);
        CHECK(m.value()->id == 9);
        CHECK(m.value()->data == QByteArrayLiteral("\x01\x02"));
    }
    // 未知类型 → 协议错误
    {
        QByteArray wire;
        wire.append(char(0x7F));
        BufferReader reader(wire);
        auto m = readDeviceMessage(reader);
        CHECK(m.isErr());
    }
    // 中途 EOF → 错误（ack 只有类型字节没有 sequence）
    {
        QByteArray wire;
        wire.append(char(kTypeAckClipboard));
        BufferReader reader(wire);
        auto m = readDeviceMessage(reader);
        CHECK(m.isErr());
    }
}

// ---- options 序列化 ----

static void testOptions()
{
    ServerOptions o;
    o.scid = 0x2AB;
    o.logLevel = LogLevel::Warn;
    o.videoCodec = VideoCodecId::H265;
    o.audio = true;
    o.videoBitRate = 4000000;
    o.maxFps = 60.0f;
    o.newDisplay = NewDisplay::make(1280, 720, 160);
    o.flexDisplay = true;

    const QVector<QString> args = o.toArgs();
    const QString joined = args.join(QLatin1Char(' '));
    CHECK(joined.contains(QStringLiteral("scid=000002ab")));
    CHECK(joined.contains(QStringLiteral("log_level=warn")));
    CHECK(joined.contains(QStringLiteral("video=true")));
    CHECK(joined.contains(QStringLiteral("audio=true")));
    CHECK(joined.contains(QStringLiteral("control=true")));
    CHECK(joined.contains(QStringLiteral("video_codec=h265")));
    CHECK(joined.contains(QStringLiteral("audio_codec=flac")));
    CHECK(joined.contains(QStringLiteral("video_bit_rate=4000000")));
    CHECK(joined.contains(QStringLiteral("max_size=0")));
    CHECK(joined.contains(QStringLiteral("scid=2ab"))); // 第二次出现：无补零十六进制
    CHECK(joined.contains(QStringLiteral("max_fps=60")));
    CHECK(joined.contains(QStringLiteral("new_display=1280x720/160")));
    CHECK(joined.contains(QStringLiteral("vd_destroy_content=true")));
    CHECK(joined.contains(QStringLiteral("vd_system_decorations=true")));
    CHECK(joined.contains(QStringLiteral("flex_display=true")));
    CHECK(joined.contains(QStringLiteral("tunnel_forward=true")));
    // 参数顺序：scid(%08x) 打头、tunnel_forward 收尾
    CHECK(args.first().startsWith(QStringLiteral("scid=")));
    CHECK(args.last() == QStringLiteral("tunnel_forward=true"));

    // socketName
    CHECK(o.socketName() == QStringLiteral("scrcpy_000002ab"));
    ServerOptions bare;
    CHECK(bare.socketName() == QStringLiteral("scrcpy"));

    // NewDisplay 参数四种形态
    CHECK(NewDisplay::make(800, 600, 240).toParam() == QStringLiteral("800x600/240"));
    CHECK(NewDisplay{}.toParam().isEmpty());
    NewDisplay onlyDpi;
    onlyDpi.dpi = 320;
    CHECK(onlyDpi.toParam() == QStringLiteral("/320"));

    // validate：非法组合
    ServerOptions bad;
    bad.video = false;
    bad.audio = false;
    CHECK(bad.validate().isErr());
    bad.audio = true;
    bad.displayId = 1;
    bad.newDisplay = NewDisplay::make(100, 100, 160);
    CHECK(bad.validate().isErr()); // display_id 与 new_display 互斥
    ServerOptions flexOnly;
    flexOnly.flexDisplay = true;
    CHECK(flexOnly.validate().isErr()); // flex_display 需要 new_display

    // serverCommand
    const QString cmd = serverCommand(QStringLiteral("/data/local/tmp/scrcpy-server.2ab"),
                                      { QStringLiteral("a=1"), QStringLiteral("b=2") });
    CHECK(cmd == QStringLiteral(
              "CLASSPATH=/data/local/tmp/scrcpy-server.2ab app_process / "
              "com.genymobile.scrcpy.Server 4.1 a=1 b=2"));

    // sessionRemotePath：无补零十六进制
    CHECK(sessionRemotePath(0x2AB) == QStringLiteral("/data/local/tmp/scrcpy-server.2ab"));

    // randomScid：31 位非负
    for (int i = 0; i < 8; ++i)
        CHECK((ServerOptions::randomScid() & 0x80000000u) == 0);
}

// ---- 视频/音频解复用 fixture（与 Rust 侧 mod tests 同构）----

static void testVideoDemux()
{
    QByteArray b;
    // session meta: flags=0x80000000, w=1280, h=720
    b += u32be(0x80000000u) + u32be(1280) + u32be(720);
    // config packet (SPS): pts=flags=CONFIG, size=3, data [1,2,3]
    b += u64be(kVideoPacketFlagConfig) + u32be(3) + QByteArrayLiteral("\x01\x02\x03");
    // keyframe: pts=1000
    b += u64be(1000 | kVideoPacketFlagKeyFrame) + u32be(2) + QByteArrayLiteral("\xAA\xBB");
    // 中途 session meta（分辨率变化 800x600）
    b += u32be(0x80000001u) + u32be(800) + u32be(600);
    // 普通 p 帧
    b += u64be(2000) + u32be(1) + QByteArrayLiteral("\xCC");

    BufferReader reader(b);
    PacketDemuxer demux(reader);
    auto p1 = demux.nextPacket();
    CHECK(p1.ok() && p1.value().has_value());
    CHECK(p1.value()->config);
    CHECK(!p1.value()->keyFrame);
    CHECK(p1.value()->data == QByteArrayLiteral("\x01\x02\x03"));
    CHECK(demux.width() == 1280 && demux.height() == 720);

    auto p2 = demux.nextPacket();
    CHECK(p2.ok() && p2.value().has_value());
    CHECK(p2.value()->keyFrame);
    CHECK(p2.value()->pts == 1000);
    CHECK(p2.value()->data == QByteArrayLiteral("\xAA\xBB"));

    auto p3 = demux.nextPacket();
    CHECK(p3.ok() && p3.value().has_value());
    CHECK(!p3.value()->config);
    CHECK(p3.value()->pts == 2000);
    CHECK(p3.value()->data == QByteArrayLiteral("\xCC"));
    CHECK(demux.width() == 800 && demux.height() == 600);

    auto p4 = demux.nextPacket();
    CHECK(p4.ok() && !p4.value().has_value()); // 流结束

    // 连接头：dummy + 64 字节设备名
    QByteArray header;
    header.append(char(0));
    QByteArray name(64, char(0));
    memcpy(name.data(), "test_device", 11);
    header += name;
    BufferReader hr(header);
    auto parsed = readConnectionHeader(hr);
    CHECK(parsed.ok());
    CHECK(parsed.value() == QStringLiteral("test_device"));
}

static void testAudioDemux()
{
    QByteArray b;
    b += u32be(audioCodecWireId(AudioCodecId::Flac));
    // config 包：flac 头
    b += u64be(kAudioPacketFlagConfig) + u32be(4) + QByteArrayLiteral("fLaC");
    // 数据包：pts=1234
    b += u64be(1234) + u32be(3) + QByteArrayLiteral("\xAA\xBB\xCC");

    BufferReader reader(b);
    // codec id 由 AudioStream::create 消费（此处跳过 4 字节模拟）
    CHECK(reader.readExact(4).value().size() == 4);
    AudioDemuxer demux(reader);
    auto p1 = demux.nextPacket();
    CHECK(p1.ok() && p1.value().has_value());
    CHECK(p1.value()->config);
    CHECK(p1.value()->data == QByteArrayLiteral("fLaC"));
    auto p2 = demux.nextPacket();
    CHECK(p2.ok() && p2.value().has_value());
    CHECK(!p2.value()->config);
    CHECK(p2.value()->pts == 1234);
    CHECK(p2.value()->data == QByteArrayLiteral("\xAA\xBB\xCC"));
    auto p3 = demux.nextPacket();
    CHECK(p3.ok() && !p3.value().has_value());

    // codec id 映射
    CHECK(audioCodecFromWire(0x6f707573).value() == AudioCodecId::Opus);
    CHECK(audioCodecFromWire(0x7FFFFFFF).isErr());
    CHECK(videoCodecFromWire(0x68323635u).value() == VideoCodecId::H265);
    CHECK(videoCodecFromWire(0x12345678).isErr());
}

// ---- Annex-B 纯函数 ----

static const char kSC4[] = "\x00\x00\x00\x01"; // 4 字节起始码（3 有效 + 前缀）

static QByteArray nal(quint8 type, const char* payload, int len)
{
    QByteArray v(kSC4, 4);
    v.append(char(type));
    v.append(payload, len);
    return v;
}

static void testAnnexB()
{
    // 3/4 字节起始码混合切分
    QByteArray stream = nal(0x67, "\x01\x02", 2); // SPS，4 字节起始码
    stream += QByteArrayLiteral("\x00\x00\x01\x68\x03"); // PPS，3 字节起始码
    stream += QByteArrayLiteral("\x00\x00\x01\x65\x09\x09");
    const QVector<QByteArray> nals = splitAnnexB(stream);
    CHECK(nals.size() == 3);
    CHECK((quint8(nals[0][0]) & 0x1f) == 7);
    CHECK((quint8(nals[1][0]) & 0x1f) == 8);
    CHECK((quint8(nals[2][0]) & 0x1f) == 5);
    CHECK(nals[0] == QByteArrayLiteral("\x67\x01\x02"));
    CHECK(nals[2] == QByteArrayLiteral("\x65\x09\x09"));
    CHECK(splitAnnexB(QByteArrayLiteral("\x01\x02\x03")).isEmpty()); // 无起始码返回空

    // AVCC 转换 + AUD(9) 剔除
    QByteArray aud = nal(0x09, "\xf0", 1);
    aud += nal(0x65, "\xAA\xBB", 2);
    QByteArray out;
    annexbToAvcc(aud, out);
    eqBytes(out, QByteArrayLiteral("\x00\x00\x00\x03\x65\xAA\xBB"), "annexb_to_avcc");

    // 参数集提取（SPS=7 / PPS=8，顺序保持）
    QByteArray ps = nal(0x67, "\x01", 1);
    ps += nal(0x68, "\x02", 1);
    ps += nal(0x65, "\x03", 1);
    const QVector<QByteArray> sets = parameterSets(ps);
    CHECK(sets.size() == 2);
    CHECK((quint8(sets[0][0]) & 0x1f) == 7);
    CHECK((quint8(sets[1][0]) & 0x1f) == 8);
}

// ---- VideoToolbox 解码回环（ffmpeg 生成 Annex-B H.264；无 ffmpeg 跳过）----

#ifdef __APPLE__
static void testVtDecodeRoundtrip()
{
    const char* ffmpeg = "/opt/homebrew/bin/ffmpeg";
    FILE* pipe = nullptr;
    if ((pipe = ::popen(
             QStringLiteral(
                 "%1 -hide_banner -loglevel error -f lavfi -i testsrc=size=160x120:rate=10 "
                 "-t 1 -pix_fmt yuv420p -c:v libx264 -f h264 pipe:1")
                 .arg(QString::fromLatin1(ffmpeg))
                 .toLocal8Bit()
                 .constData(),
             "r"))
        == nullptr) {
        qInfo() << "[mw_scrcpy] 无 ffmpeg，跳过 VT 回环测试";
        return;
    }
    QByteArray stream;
    char buf[16384];
    size_t n;
    while ((n = ::fread(buf, 1, sizeof(buf), pipe)) > 0)
        stream.append(buf, int(n));
    const int rc = ::pclose(pipe);
    if (rc != 0 || stream.isEmpty()) {
        qInfo() << "[mw_scrcpy] ffmpeg 生成失败，跳过 VT 回环测试";
        return;
    }

    const QVector<QByteArray> nals = splitAnnexB(stream);
    CHECK(nals.size() >= 4);

    // config = SPS+PPS（Annex-B，与 scrcpy server 的 config 包同构）
    QByteArray config;
    for (const QByteArray& nalUnit : nals) {
        const quint8 t = quint8(nalUnit[0]) & 0x1f;
        if (t == 7 || t == 8) {
            config += QByteArray(kSC4, 4);
            config += nalUnit;
        }
    }
    auto dec = VideoToolboxDecoder::create(config);
    if (!dec.ok()) {
        qInfo().noquote() << "[mw_scrcpy] VT 会话建立失败，跳过:" << dec.error();
        return;
    }
    CHECK(dec.value().width() == 160);
    CHECK(dec.value().height() == 120);

    quint32 submitted = 0;
    for (const QByteArray& nalUnit : nals) {
        const quint8 t = quint8(nalUnit[0]) & 0x1f;
        if (t == 1 || t == 5) {
            QByteArray frame(kSC4, 4);
            frame += nalUnit;
            auto r = dec.value().decode(quint64(submitted) * 100000, frame);
            CHECK(r.ok());
            ++submitted;
        }
    }
    CHECK(submitted >= 8);

    // VT 异步输出：轮询最多 2s
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    std::optional<DecodedFrame> first;
    while (std::chrono::steady_clock::now() < deadline && !first.has_value()) {
        while (auto f = dec.value().drain()) {
            if (!first.has_value())
                first = std::move(f);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(first.has_value());
    if (first.has_value()) {
        CHECK(first->width() == 160);
        CHECK(first->height() == 120);
        auto nv12 = first->toNv12();
        CHECK(nv12.ok());
        if (nv12.ok()) {
            CHECK(nv12.value().data.size() == nv12.value().yStride * 120 + nv12.value().uvStride * 60);
            CHECK(nv12.value().yStride >= 160 && nv12.value().uvStride >= 160);
        }
    }
    CHECK(dec.value().decodedCount() >= 8);

    // reconfigure：同参数重建会话
    auto re = dec.value().reconfigure(config);
    CHECK(re.ok());
    CHECK(dec.value().width() == 160);
}
#endif // __APPLE__

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    testControlMessageEncode();
    testDeviceMessage();
    testOptions();
    testVideoDemux();
    testAudioDemux();
    testAnnexB();
#ifdef __APPLE__
    testVtDecodeRoundtrip();
#endif

    if (g_failures == 0) {
        qInfo().noquote() << QStringLiteral("mw_scrcpy_test: 全部通过");
        return 0;
    }
    qCritical().noquote() << QStringLiteral("mw_scrcpy_test: %1 项失败").arg(g_failures);
    return 1;
}
