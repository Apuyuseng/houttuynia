// VmConfig 实现：~/.mwine/config.json 读写 + 校验（移植自 mwine-core/src/config.rs）。
#include "config.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QTemporaryFile>

#include <algorithm>

#include "fs.hpp"
#include "json.hpp"

namespace mw::config {
namespace {

// 原子写：与 mw::writeFile 相同配方，外加 setAutoRemove(false)。
// 原因：本机 Qt 6.11.1 下 QTemporaryFile::rename() 后析构仍会按 autoRemove
// 删除已落位的目标文件（独立复现见任务记录），mw::writeFile 因此丢数据；
// common 禁止修改，故模块内置等价实现（common 侧修复后可换回）。
mw::Result<bool> atomicWrite(const QString& path, const QByteArray& data)
{
    QTemporaryFile tmp(QFileInfo(path).absolutePath() + QStringLiteral("/.mwtmp-XXXXXX"));
    if (!tmp.open())
        return mw::Result<bool>::Err(
            QStringLiteral("无法创建临时文件: %1").arg(tmp.errorString()));
    if (tmp.write(data) != data.size() || !tmp.flush())
        return mw::Result<bool>::Err(QStringLiteral("写入失败: %1").arg(tmp.errorString()));
    if (QFileInfo::exists(path) && !QFile::remove(path))
        return mw::Result<bool>::Err(QStringLiteral("无法替换 %1").arg(path));
    tmp.setAutoRemove(false); // 关键：否则析构会连 rename 后的落位文件一起删掉
    if (!tmp.rename(path))
        return mw::Result<bool>::Err(
            QStringLiteral("无法落位 %1: %2").arg(path, tmp.errorString()));
    return mw::Result<bool>::Ok();
}

// ---- ASCII 判定（对应 Rust char::is_ascii_*；不用 QChar 的 Unicode 语义）----
bool isAsciiDigit(char16_t c) { return c >= u'0' && c <= u'9'; }
bool isAsciiHexDigit(char16_t c)
{
    return isAsciiDigit(c) || (c >= u'a' && c <= u'f') || (c >= u'A' && c <= u'F');
}
bool isAsciiAlnum(char16_t c)
{
    return isAsciiDigit(c) || (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z');
}
bool isAsciiLower(char16_t c) { return c >= u'a' && c <= u'z'; }
bool isAsciiUpper(char16_t c) { return c >= u'A' && c <= u'Z'; }

template <typename Pred>
bool allChars(const QString& s, Pred pred)
{
    for (const QChar c : s)
        if (!pred(c.unicode()))
            return false;
    return true;
}

// JSON 字段读取：缺失/类型不符 → 默认值（对应 serde default 的宽容读法）
QString strField(const QJsonObject& o, const char* k, const QString& d)
{
    const auto v = o.value(QLatin1String(k));
    return v.isString() ? v.toString() : d;
}

quint32 numField(const QJsonObject& o, const char* k, quint32 d)
{
    return quint32(mw::jNum(o, k, double(d)));
}

bool boolField(const QJsonObject& o, const char* k, bool d)
{
    return mw::jBool(o, k, d);
}

} // namespace

QString VmConfig::path()
{
    return QDir(mw::mwineDir()).filePath(QStringLiteral("config.json"));
}

VmConfig VmConfig::load()
{
    // 对应 Rust：read_to_string().ok().and_then(parse ok).unwrap_or_default()
    const auto r = mw::readFile(path());
    if (!r)
        return VmConfig{};
    bool ok = false;
    const QJsonObject o = mw::parseJson(r.value(), &ok);
    if (!ok)
        return VmConfig{};
    return fromJson(o);
}

mw::Result<bool> VmConfig::save() const
{
    QDir().mkpath(mw::mwineDir());
    // pretty 输出（对应 serde_json::to_string_pretty）；原子写
    return atomicWrite(path(), mw::toJsonBytes(toJson(), false));
}

Lcd VmConfig::lcd() const
{
    if (mode == QLatin1String("phone"))
        return { 1080, 2400, 440 };
    if (mode == QLatin1String("tablet"))
        return { 2560, 1600, 320 };
    if (mode == QLatin1String("tv"))
        return { 1920, 1080, 320 };
    return { int(width), int(height), int(dpi) }; // custom 及未知模式
}

QJsonObject VmConfig::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("mode"), mode);
    o.insert(QStringLiteral("width"), double(width));
    o.insert(QStringLiteral("height"), double(height));
    o.insert(QStringLiteral("dpi"), double(dpi));
    o.insert(QStringLiteral("memory_mb"), double(memoryMb));
    o.insert(QStringLiteral("sdcard_path"), sdcardPath);
    o.insert(QStringLiteral("share_dir"), shareDir);
    o.insert(QStringLiteral("avd"), avd);
    o.insert(QStringLiteral("proxy"), proxy);
    o.insert(QStringLiteral("cpu_cores"), double(cpuCores));
    o.insert(QStringLiteral("gpu_mode"), gpuMode);
    o.insert(QStringLiteral("qemu_extra"), qemuExtra);
    o.insert(QStringLiteral("netspeed"), netspeed);
    o.insert(QStringLiteral("netdelay"), netdelay);
    o.insert(QStringLiteral("no_snapshot"), noSnapshot);
    o.insert(QStringLiteral("no_boot_anim"), noBootAnim);
    o.insert(QStringLiteral("camera_back"), cameraBack);
    o.insert(QStringLiteral("camera_front"), cameraFront);
    o.insert(QStringLiteral("allow_host_audio"), allowHostAudio);
    o.insert(QStringLiteral("dns_servers"), dnsServers);
    o.insert(QStringLiteral("vsync_rate"), double(vsyncRate));
    o.insert(QStringLiteral("no_snapshot_save"), noSnapshotSave);
    o.insert(QStringLiteral("idle_stop_min"), double(idleStopMin));
    o.insert(QStringLiteral("hide_emulator_window"), hideEmulatorWindow);
    o.insert(QStringLiteral("window_mode_all"), windowModeAll);
    o.insert(QStringLiteral("window_borderless"), windowBorderless);
    o.insert(QStringLiteral("phone_number"), phoneNumber);
    o.insert(QStringLiteral("no_sim"), noSim);
    o.insert(QStringLiteral("serialno"), serialno);
    o.insert(QStringLiteral("locale"), locale);
    o.insert(QStringLiteral("hw_decoder_off"), hwDecoderOff);
    o.insert(QStringLiteral("vulkan_off"), vulkanOff);
    o.insert(QStringLiteral("gldma_off"), gldmaOff);
    o.insert(QStringLiteral("virtio_input_off"), virtioInputOff);
    o.insert(QStringLiteral("mouse_as_touch"), mouseAsTouch);
    o.insert(QStringLiteral("hw_keyboard"), hwKeyboard);
    o.insert(QStringLiteral("selinux"), selinux);
    o.insert(QStringLiteral("tcpdump"), tcpdump);
    o.insert(QStringLiteral("detect_image_hang"), detectImageHang);
    o.insert(QStringLiteral("writable_system"), writableSystem);
    return o;
}

VmConfig VmConfig::fromJson(const QJsonObject& o)
{
    VmConfig c; // 全默认；缺字段/类型不符即保留默认（serde default）
    c.mode = strField(o, "mode", c.mode);
    c.width = numField(o, "width", c.width);
    c.height = numField(o, "height", c.height);
    c.dpi = numField(o, "dpi", c.dpi);
    c.memoryMb = numField(o, "memory_mb", c.memoryMb);
    c.sdcardPath = strField(o, "sdcard_path", c.sdcardPath);
    c.shareDir = strField(o, "share_dir", c.shareDir);
    c.avd = strField(o, "avd", c.avd);
    c.proxy = strField(o, "proxy", c.proxy);
    c.cpuCores = numField(o, "cpu_cores", c.cpuCores);
    c.gpuMode = strField(o, "gpu_mode", c.gpuMode);
    if (c.gpuMode.trimmed().isEmpty())
        c.gpuMode = QStringLiteral("host"); // 旧配置存过空串（=qemu 软渲默认），按新默认 host
    c.qemuExtra = strField(o, "qemu_extra", c.qemuExtra);
    c.netspeed = strField(o, "netspeed", c.netspeed);
    c.netdelay = strField(o, "netdelay", c.netdelay);
    c.noSnapshot = boolField(o, "no_snapshot", c.noSnapshot);
    c.noBootAnim = boolField(o, "no_boot_anim", c.noBootAnim);
    c.cameraBack = strField(o, "camera_back", c.cameraBack);
    c.cameraFront = strField(o, "camera_front", c.cameraFront);
    c.allowHostAudio = boolField(o, "allow_host_audio", c.allowHostAudio);
    c.dnsServers = strField(o, "dns_servers", c.dnsServers);
    c.vsyncRate = numField(o, "vsync_rate", c.vsyncRate);
    c.noSnapshotSave = boolField(o, "no_snapshot_save", c.noSnapshotSave);
    c.idleStopMin = numField(o, "idle_stop_min", c.idleStopMin);
    c.hideEmulatorWindow = boolField(o, "hide_emulator_window", c.hideEmulatorWindow);
    c.windowModeAll = boolField(o, "window_mode_all", c.windowModeAll);
    c.windowBorderless = boolField(o, "window_borderless", c.windowBorderless);
    c.phoneNumber = strField(o, "phone_number", c.phoneNumber);
    c.noSim = boolField(o, "no_sim", c.noSim);
    c.serialno = strField(o, "serialno", c.serialno);
    c.locale = strField(o, "locale", c.locale);
    c.hwDecoderOff = boolField(o, "hw_decoder_off", c.hwDecoderOff);
    c.vulkanOff = boolField(o, "vulkan_off", c.vulkanOff);
    c.gldmaOff = boolField(o, "gldma_off", c.gldmaOff);
    c.virtioInputOff = boolField(o, "virtio_input_off", c.virtioInputOff);
    c.mouseAsTouch = boolField(o, "mouse_as_touch", c.mouseAsTouch);
    c.hwKeyboard = boolField(o, "hw_keyboard", c.hwKeyboard);
    c.selinux = strField(o, "selinux", c.selinux);
    c.tcpdump = boolField(o, "tcpdump", c.tcpdump);
    c.detectImageHang = boolField(o, "detect_image_hang", c.detectImageHang);
    c.writableSystem = boolField(o, "writable_system", c.writableSystem);
    return c;
}

mw::Result<bool> VmConfig::validate() const
{
    // 摄像头取值
    const QString cams[2] = { cameraBack, cameraFront };
    const QString camLabels[2] = { QStringLiteral("后置摄像头"), QStringLiteral("前置摄像头") };
    for (int i = 0; i < 2; ++i) {
        const QString& v = cams[i];
        if (v.isEmpty() || v == QLatin1String("none") || v == QLatin1String("emulated")
            || v == QLatin1String("virtualscene") || v == QLatin1String("webcam0"))
            continue;
        return mw::err<bool>(QStringLiteral("%1取值无效: %2").arg(camLabels[i], v));
    }

    // DNS：逗号分隔，段内 IPv4（4 段纯数字）或冒号分隔非空十六进制
    //（照抄 Rust 语义：含 "::" 压缩的 IPv6 会因空段被拒）
    if (!dnsServers.isEmpty()) {
        for (QString part : dnsServers.split(QLatin1Char(','))) {
            const QString s = part.trimmed();
            const QStringList dotParts = s.split(QLatin1Char('.'));
            const bool ipv4 = dotParts.size() == 4
                && std::all_of(dotParts.cbegin(), dotParts.cend(), [](const QString& p) {
                       return !p.isEmpty() && allChars(p, isAsciiDigit);
                   });
            const QStringList colonParts = s.split(QLatin1Char(':'));
            const bool ipv6 = std::all_of(
                colonParts.cbegin(), colonParts.cend(), [](const QString& p) {
                    return !p.isEmpty() && allChars(p, isAsciiHexDigit);
                });
            if ((!ipv4 && !ipv6) || s.isEmpty())
                return mw::err<bool>(
                    QStringLiteral("DNS 地址无效: %1（IPv4/IPv6，逗号分隔）").arg(s));
        }
    }

    // 刷新率
    if (vsyncRate != 0 && vsyncRate != 60 && vsyncRate != 90 && vsyncRate != 120)
        return mw::err<bool>(QStringLiteral("刷新率仅支持 60/90/120 Hz: %1").arg(vsyncRate));

    // 手机号
    if (!phoneNumber.isEmpty()
        && !allChars(phoneNumber,
                     [](char16_t c) { return isAsciiDigit(c) || c == u'+' || c == u'-'; }))
        return mw::err<bool>(QStringLiteral("手机号仅允许数字 / + / -"));

    // -android-serialno 要求 ^[a-zA-Z0-9._-,]+$
    if (!serialno.isEmpty()
        && !allChars(serialno, [](char16_t c) {
               return isAsciiAlnum(c) || c == u'.' || c == u'_' || c == u'-' || c == u',';
           }))
        return mw::err<bool>(QStringLiteral("序列号仅允许字母/数字/._-,"));

    // 语言：形如 zh / zh-CN；值会进 guest shell，拒绝其余字符
    if (!locale.isEmpty()) {
        const QStringList parts = locale.split(QLatin1Char('-'));
        const QString lang = parts.value(0);
        const QString region = parts.value(1); // 无第二段时为空
        const bool hasRegion = parts.size() >= 2;
        const bool langOk = lang.size() >= 2 && lang.size() <= 3 && allChars(lang, isAsciiLower);
        const bool regionOk =
            !hasRegion || (region.size() == 2 && allChars(region, isAsciiUpper));
        if (!langOk || !regionOk || parts.size() > 2)
            return mw::err<bool>(QStringLiteral("语言格式应为 xx 或 xx-XX: %1").arg(locale));
    }

    // SELinux
    if (!selinux.isEmpty() && selinux != QLatin1String("permissive")
        && selinux != QLatin1String("disabled"))
        return mw::err<bool>(QStringLiteral("SELinux 取值无效: %1").arg(selinux));

    return mw::Result<bool>::Ok();
}

} // namespace mw::config
