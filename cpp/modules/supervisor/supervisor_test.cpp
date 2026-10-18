// supervisor 模块自测：tar 流构造+解包 roundtrip（内存）、AVD config.ini 读写、
// qemu 参数拼装/快照参数、zip 解包、sha256/稀疏化、仓库 XML 解析。
// 不发网络、不碰真 SDK（全部走 QTemporaryDir）。
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtGlobal>

#include <zstd.h>
#include <zlib.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <string_view>

#include "supervisor.hpp"

using namespace mw::supervisor;

static int g_failed = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            qWarning().noquote() << QStringLiteral("CHECK 失败: %1 (%2:%3)")       \
                                        .arg(QStringLiteral(#cond),              \
                                            QStringLiteral(__FILE__),            \
                                            QString::number(__LINE__));         \
            ++g_failed;                                                        \
        }                                                                      \
    } while (0)

// ===================== 测试用 tar 构造器（ustar + GNU 'L' + pax 'x'） =====================

static void setOctalField(char* f, int len, quint64 v)
{
    std::memset(f, ' ', size_t(len));
    char tmp[32];
    const int n = std::snprintf(tmp, sizeof tmp, "%llo", static_cast<unsigned long long>(v));
    std::memcpy(f, tmp, size_t(n));
    f[n] = '\0';
}

static QByteArray tarHeader(const QByteArray& name, char type, quint64 size,
                            const QByteArray& link, quint32 mode)
{
    QByteArray h(512, '\0');
    char* p = h.data();
    std::memcpy(p, name.constData(), size_t(std::min<int>(99, name.size())));
    setOctalField(p + 100, 8, mode);
    setOctalField(p + 108, 8, 0);
    setOctalField(p + 116, 8, 0);
    setOctalField(p + 124, 12, size);
    setOctalField(p + 136, 12, 0);
    std::memset(p + 148, ' ', 8);
    p[156] = type;
    if (!link.isEmpty())
        std::memcpy(p + 157, link.constData(), size_t(std::min<int>(99, link.size())));
    std::memcpy(p + 257, "ustar", 5);
    p[263] = '0';
    p[264] = '0';
    quint64 sum = 0;
    for (int i = 0; i < 512; ++i)
        sum += (i >= 148 && i < 156) ? quint64(' ') : quint64(quint8(p[i]));
    setOctalField(p + 148, 8, sum);
    return h;
}

static void appendPayload(QByteArray& tar, const QByteArray& data)
{
    tar += data;
    const int pad = (512 - (data.size() % 512)) % 512;
    tar += QByteArray(pad, '\0');
}

static void appendEntry(QByteArray& tar, const QByteArray& name, char type,
                        const QByteArray& data, const QByteArray& link = {},
                        quint32 mode = 0644)
{
    QByteArray usedName = name;
    if (name.size() > 99) { // GNU 长名：'L' 扩展 + 截断名占位
        const QByteArray ln = name + '\0';
        tar += tarHeader(QByteArray("././@LongLink"), 'L', quint64(ln.size()), {}, 0644);
        appendPayload(tar, ln);
        usedName = name.left(40);
    }
    tar += tarHeader(usedName, type, quint64(data.size()), link, mode);
    appendPayload(tar, data);
}

// pax 'x' 条目：path 记录覆盖下一 entry 的名字（记录 = "<len> <key>=<val>\n"，
// len 含数字自身、空格与结尾换行）
static void appendPaxPath(QByteArray& tar, const QByteArray& longPath)
{
    const QByteArray body = " path=" + longPath + "\n";
    int len = body.size() + 2;
    while (int(QByteArray::number(len).size()) + int(body.size()) != len)
        ++len;
    const QByteArray rec = QByteArray::number(len) + body;
    tar += tarHeader(QByteArray("PaxHeaders/0/f"), 'x', quint64(rec.size()), {}, 0644);
    appendPayload(tar, rec);
}

static QByteArray buildSampleTar()
{
    QByteArray tar;
    appendEntry(tar, "pkg/", '5', {});
    // manifest.json
    appendEntry(tar, "pkg/manifest.json", '0',
                R"({"version":"1.2.3","android_api":33,"files":{"a":"00"}})", {}, 0644);
    // 大文件（多分片）
    QByteArray big(700000, '\0');
    for (int i = 0; i < big.size(); ++i)
        big[i] = char((i * 31 + 7) & 0xff);
    appendEntry(tar, "pkg/arm64-v8a/system-qemu.img", '0', big, {}, 0644);
    // 空文件
    appendEntry(tar, "pkg/arm64-v8a/empty.txt", '0', QByteArray(), {}, 0644);
    // 硬链接
    appendEntry(tar, "pkg/arm64-v8a/hard.bin", '1', {}, "pkg/arm64-v8a/system-qemu.img", 0644);
    // 符号链接
    appendEntry(tar, "pkg/arm64-v8a/link.img", '2', {}, "../arm64-v8a/system-qemu.img", 0777);
    // GNU 长名（>100 字节）
    const QByteArray longName =
        "pkg/arm64-v8a/some/really/deeply/nested/directory/structure/with/a/very/long/"
        "trailing/file/name/that/exceeds/one/hundred/bytes/data.bin";
    appendEntry(tar, longName, '0', QByteArray("LONGNAME-OK"), {}, 0600);
    // pax path 覆盖
    appendPaxPath(tar, "pkg/arm64-v8a/pax-renamed.txt");
    appendEntry(tar, "pkg/short.txt", '0', QByteArray("PAX-OK"));
    tar += QByteArray(1024, '\0'); // 两个结束零块
    return tar;
}

static void verifyExtracted(const QString& root)
{
    const QDir d(root);
    CHECK(QFileInfo::exists(d.filePath(QStringLiteral("pkg/manifest.json"))));
    {
        QFile f(d.filePath(QStringLiteral("pkg/manifest.json")));
        CHECK(f.open(QIODevice::ReadOnly));
        CHECK(f.readAll().contains("\"android_api\":33"));
    }
    // 大文件内容
    {
        QFile f(d.filePath(QStringLiteral("pkg/arm64-v8a/system-qemu.img")));
        CHECK(f.open(QIODevice::ReadOnly));
        const QByteArray got = f.readAll();
        CHECK(got.size() == 700000);
        CHECK(quint8(got.at(1)) == quint8((1 * 31 + 7) & 0xff));
        CHECK(quint8(got.at(699999)) == quint8((699999 * 31 + 7) & 0xff));
    }
    // 空文件
    CHECK(QFileInfo(d.filePath(QStringLiteral("pkg/arm64-v8a/empty.txt"))).size() == 0);
    // 硬链接内容可达
    {
        QFile f(d.filePath(QStringLiteral("pkg/arm64-v8a/hard.bin")));
        CHECK(f.open(QIODevice::ReadOnly));
        CHECK(f.size() == 700000);
    }
    // 符号链接保留
    const QFileInfo lnk(d.filePath(QStringLiteral("pkg/arm64-v8a/link.img")));
    CHECK(lnk.isSymbolicLink());
    CHECK(lnk.symLinkTarget().endsWith(QStringLiteral("system-qemu.img")));
    // GNU 长名
    {
        QFile f(d.filePath(QStringLiteral(
            "pkg/arm64-v8a/some/really/deeply/nested/directory/structure/with/a/very/long/"
            "trailing/file/name/that/exceeds/one/hundred/bytes/data.bin")));
        CHECK(f.open(QIODevice::ReadOnly));
        CHECK(f.readAll() == QByteArray("LONGNAME-OK"));
    }
    // 'L' 扩展生效：头部的截断占位名不应被当成实体文件落地
    CHECK(!QFileInfo::exists(
        d.filePath(QStringLiteral("pkg/arm64-v8a/some/really/deeply/nested/d"))));
    // pax path 覆盖
    {
        QFile f(d.filePath(QStringLiteral("pkg/arm64-v8a/pax-renamed.txt")));
        CHECK(f.open(QIODevice::ReadOnly));
        CHECK(f.readAll() == QByteArray("PAX-OK"));
    }
}

static void testTarRoundtrip()
{
    const QByteArray tar = buildSampleTar();

    // 1) 直接内存流式喂入（7 字节奇数分片，验证状态机跨块）
    {
        QTemporaryDir dir;
        TarStreamParser parser(dir.path());
        bool ok = true;
        for (int i = 0; i < tar.size() && ok; i += 7) {
            auto r = parser.feed(tar.constData() + i, std::min(7, int(tar.size() - i)));
            if (!r) {
                qWarning().noquote() << r.error();
                ok = false;
            }
        }
        CHECK(ok);
        if (ok) {
            auto fr = parser.finish();
            CHECK(fr);
            CHECK(parser.entries().size() >= 8);
            verifyExtracted(dir.path());
        }
    }

    // 2) tar.zst 全链路（zstd 压缩 → extractTarZst 解包落盘）
    {
        QTemporaryDir dir;
        const QString zst = QDir(dir.path()).filePath(QStringLiteral("pkg.tar.zst"));
        QByteArray comp(long(ZSTD_compressBound(size_t(tar.size()))) + 16, '\0');
        const size_t n = ZSTD_compress(comp.data(), comp.size(), tar.constData(),
                                       size_t(tar.size()), 3);
        CHECK(!ZSTD_isError(n));
        comp.resize(int(n));
        QFile zf(zst);
        CHECK(zf.open(QIODevice::WriteOnly));
        zf.write(comp);
        zf.close();

        const QString out = QDir(dir.path()).filePath(QStringLiteral("out"));
        QDir().mkpath(out);
        auto r = extractTarZst(zst, out);
        CHECK(r);
        if (r)
            verifyExtracted(out);
    }
}

static void testTarRejects()
{
    // 校验和篡改
    {
        QByteArray tar = buildSampleTar();
        tar[10] = char(quint8(tar[10]) ^ 0xff);
        QTemporaryDir dir;
        TarStreamParser parser(dir.path());
        CHECK(!parser.feed(tar.constData(), tar.size()));
    }
    // 路径穿越
    {
        QByteArray tar;
        appendEntry(tar, "../evil.txt", '0', QByteArray("x"));
        QTemporaryDir dir;
        TarStreamParser parser(dir.path());
        CHECK(!parser.feed(tar.constData(), tar.size()));
    }
    // 流截断（条目中间）
    {
        QByteArray tar;
        appendEntry(tar, "big.bin", '0', QByteArray(5000, 'x'));
        tar.chop(700); // 数据没给完
        QTemporaryDir dir;
        TarStreamParser parser(dir.path());
        CHECK(parser.feed(tar.constData(), tar.size())); // 解析阶段不报错
        CHECK(!parser.finish());                          // 收尾时发现截断
    }
}

// ===================== zip 构造 + 解包 =====================

static QByteArray rawDeflate(const QByteArray& d)
{
    z_stream zs{};
    deflateInit2(&zs, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
    QByteArray out(d.size() + d.size() / 2 + 256, '\0');
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(d.constData()));
    zs.avail_in = uint(d.size());
    zs.next_out = reinterpret_cast<Bytef*>(out.data());
    zs.avail_out = uint(out.size());
    deflate(&zs, Z_FINISH);
    out.resize(int(out.size() - zs.avail_out));
    deflateEnd(&zs);
    return out;
}

static void put32(QByteArray& b, quint32 v)
{
    b.append(char(quint8(v))).append(char(quint8(v >> 8))).append(char(quint8(v >> 16)))
        .append(char(quint8(v >> 24)));
}
static void put16(QByteArray& b, quint16 v)
{
    b.append(char(quint8(v))).append(char(quint8(v >> 8)));
}

static void testZipRoundtrip()
{
    struct E
    {
        QByteArray name;
        QByteArray data;
        int method;
    };
    const QVector<E> entries = {
        { QByteArray("arm64-v8a/"), QByteArray(), 0 },
        { QByteArray("arm64-v8a/system.img"), QByteArray(300000, '\x5a'), 8 },
        { QByteArray("arm64-v8a/source.properties"), QByteArray("Pkg.Desc=Image\n"), 0 },
    };

    QByteArray local;
    struct Cd
    {
        QByteArray name;
        quint32 crc, csize, usize, lho;
        quint16 method;
    };
    QVector<Cd> cds;
    for (const auto& e : entries) {
        const quint32 crc = quint32(
            ::crc32(crc32(0, Z_NULL, 0),
                    reinterpret_cast<const Bytef*>(e.data.constData()), uint(e.data.size())));
        const QByteArray comp = e.method == 0 ? e.data : rawDeflate(e.data);
        const quint32 lho = quint32(local.size());
        put32(local, 0x04034b50);
        put16(local, 20);
        put16(local, 0);
        put16(local, quint16(e.method));
        put16(local, 0);
        put16(local, 0);
        put32(local, crc);
        put32(local, quint32(comp.size()));
        put32(local, quint32(e.data.size()));
        put16(local, quint16(e.name.size()));
        put16(local, 0);
        local += e.name;
        local += comp;
        cds.push_back({ e.name, crc, quint32(comp.size()), quint32(e.data.size()), lho,
                        quint16(e.method) });
    }
    const quint32 cdOfs = quint32(local.size());
    QByteArray cd;
    for (const auto& c : cds) {
        put32(cd, 0x02014b50);
        put16(cd, 20);
        put16(cd, 20);
        put16(cd, 0);
        put16(cd, c.method);
        put16(cd, 0);
        put16(cd, 0);
        put32(cd, c.crc);
        put32(cd, c.csize);
        put32(cd, c.usize);
        put16(cd, quint16(c.name.size()));
        put16(cd, 0);
        put16(cd, 0);
        put16(cd, 0);
        put16(cd, 0);
        put32(cd, 0);
        put32(cd, c.lho);
        cd += c.name;
    }
    QByteArray zip = local + cd;
    put32(zip, 0x06054b50);
    put16(zip, 0);
    put16(zip, 0);
    put16(zip, quint16(cds.size()));
    put16(zip, quint16(cds.size()));
    put32(zip, quint32(cd.size()));
    put32(zip, cdOfs);
    put16(zip, 0);

    QTemporaryDir dir;
    const QString zipPath = QDir(dir.path()).filePath(QStringLiteral("img.zip"));
    QFile zf(zipPath);
    CHECK(zf.open(QIODevice::WriteOnly));
    zf.write(zip);
    zf.close();

    const QString out = QDir(dir.path()).filePath(QStringLiteral("out"));
    auto r = zipExtract(zipPath, out);
    CHECK(r);
    if (r) {
        CHECK(QFileInfo::exists(QDir(out).filePath(QStringLiteral("arm64-v8a/system.img"))));
        {
            QFile f(QDir(out).filePath(QStringLiteral("arm64-v8a/system.img")));
            CHECK(f.open(QIODevice::ReadOnly));
            CHECK(f.size() == 300000);
            CHECK(f.readAll() == QByteArray(300000, '\x5a'));
        }
        {
            QFile f(QDir(out).filePath(QStringLiteral("arm64-v8a/source.properties")));
            CHECK(f.open(QIODevice::ReadOnly));
            CHECK(f.readAll() == QByteArray("Pkg.Desc=Image\n"));
        }
    }
}

// ===================== AVD config.ini 读写 =====================

static AvdInfo writeCfg(const QString& dir, const QByteArray& body)
{
    QDir().mkpath(dir);
    QFile f(QDir(dir).filePath(QStringLiteral("config.ini")));
    (void)f.open(QIODevice::WriteOnly | QIODevice::Truncate);
    f.write(body);
    f.close();
    AvdInfo avd;
    avd.name = QStringLiteral("t");
    avd.path = dir;
    return avd;
}

static QByteArray readCfg(const QString& dir)
{
    QFile f(QDir(dir).filePath(QStringLiteral("config.ini")));
    (void)f.open(QIODevice::ReadOnly);
    const QByteArray b = f.readAll();
    f.close();
    return b;
}

static void testApplyDeviceMode()
{
    // 在位改写
    {
        QTemporaryDir d;
        const auto avd =
            writeCfg(d.path(), "hw.lcd.width = 1080\nhw.lcd.height = 2340\nhw.lcd.density = "
                               "420\nhw.device.name = pixel\n");
        CHECK(applyDeviceMode(avd, DeviceMode::Tablet));
        const QByteArray out = readCfg(d.path());
        CHECK(out.contains("hw.lcd.width = 2560"));
        CHECK(out.contains("hw.lcd.height = 1600"));
        CHECK(out.contains("hw.lcd.density = 320"));
        CHECK(out.contains("hw.device.name = mwine_tablet"));
        CHECK(!out.contains("hw.lcd.width = 1080"));
        CHECK(!out.contains("hw.device.name = pixel"));

        CHECK(applyDeviceMode(avd, DeviceMode::Phone));
        const QByteArray out2 = readCfg(d.path());
        CHECK(out2.contains("hw.lcd.width = 1080"));
        CHECK(out2.contains("hw.lcd.density = 440"));
        CHECK(out2.contains("hw.device.name = mwine_phone"));
    }
    // 缺键补齐
    {
        QTemporaryDir d;
        const auto avd = writeCfg(d.path(), "abi.type = arm64-v8a\n");
        CHECK(applyDeviceMode(avd, DeviceMode::Phone));
        const QByteArray out = readCfg(d.path());
        CHECK(out.contains("abi.type = arm64-v8a"));
        CHECK(out.contains("hw.lcd.width = 1080"));
        CHECK(out.contains("hw.device.name = mwine_phone"));
    }
    // LCD 参数合理性（对齐 Rust lcd_profiles_are_sane）
    const LcdSpec p = lcdSpec(DeviceMode::Phone);
    const LcdSpec t = lcdSpec(DeviceMode::Tablet);
    CHECK(p.h > p.w);
    CHECK(t.w > t.h);
    CHECK(t.w * 160 / t.dpi >= 600);
    CHECK(p.w * 160 / p.dpi < 600);
    CHECK(std::string_view(deviceName(DeviceMode::Phone)) == "mwine_phone");
}

static void testSetLcd()
{
    QTemporaryDir d;
    const auto avd = writeCfg(d.path(), "hw.lcd.width = 1080\nhw.lcd.height = 2340\nhw.lcd."
                                        "density = 420\nhw.initialOrientation = portrait\n");
    // 预置 hardware-qemu.ini（emulator 运行时写回、启动时优先生效——必须同步覆盖）
    QFile hwq(QDir(d.path()).filePath(QStringLiteral("hardware-qemu.ini")));
    (void)hwq.open(QIODevice::WriteOnly | QIODevice::Truncate);
    hwq.write("hw.lcd.width = 999\nhw.lcd.height = 888\n");
    hwq.close();

    CHECK(setLcd(avd, 720, 1280, 240));
    const QByteArray out = readCfg(d.path());
    CHECK(out.contains("hw.lcd.width = 720"));
    CHECK(out.contains("hw.lcd.height = 1280"));
    CHECK(out.contains("hw.lcd.density = 240"));
    CHECK(out.contains("hw.initialOrientation = portrait")); // 720 < 1280
    // setLcd 缺键补 orientation 之外的 lcd 键
    {
        QTemporaryDir d2;
        const auto avd2 = writeCfg(d2.path(), "abi.type = arm64-v8a\n");
        CHECK(setLcd(avd2, 2560, 1600, 320));
        const QByteArray o = readCfg(d2.path());
        CHECK(o.contains("hw.lcd.width = 2560"));
        CHECK(o.contains("hw.initialOrientation = landscape")); // 2560 > 1600
    }
    QFile hwq2(QDir(d.path()).filePath(QStringLiteral("hardware-qemu.ini")));
    (void)hwq2.open(QIODevice::ReadOnly);
    const QByteArray hwout = hwq2.readAll();
    hwq2.close();
    CHECK(hwout.contains("hw.lcd.width = 720"));
    CHECK(hwout.contains("hw.lcd.height = 1280"));
    CHECK(!hwout.contains("999")); // 旧值已被覆盖
    // hardware-qemu.ini 无 density 键时不补写（仅在位替换，对齐 Rust）
}

// ===================== qemu 参数拼装（快照参数） =====================

static void testBuildEmuArgs()
{
    // AVD 模式 + 快照
    {
        LaunchOptions o;
        o.snapshot = QStringLiteral("quick1");
        o.noBootAnim = true;
        o.proxy = QStringLiteral("http://127.0.0.1:7890");
        o.sdcard = QStringLiteral("/tmp/sd.img");
        o.extraArgs = { QStringLiteral("-cores"), QStringLiteral("4") };
        o.wifiSsid = QStringLiteral("ChinaNet-abcd");
        o.memoryMb = 1536;
        const QStringList args = buildEmuArgs(QStringLiteral("Pixel_33"), o, std::nullopt);
        CHECK(args.indexOf(QStringLiteral("-avd")) >= 0);
        CHECK(args.at(args.indexOf(QStringLiteral("-avd")) + 1) == QStringLiteral("Pixel_33"));
        const int snap = args.indexOf(QStringLiteral("-snapshot"));
        CHECK(snap >= 0);
        CHECK(args.at(snap + 1) == QStringLiteral("quick1"));
        CHECK(snap < args.indexOf(QStringLiteral("-avd"))); // 快照参数在 -avd 之前（对齐 Rust 顺序）
        CHECK(args.contains(QStringLiteral("-no-window")));
        CHECK(args.contains(QStringLiteral("-no-audio")));
        CHECK(args.contains(QStringLiteral("-no-boot-anim")));
        CHECK(args.at(args.indexOf(QStringLiteral("-http-proxy")) + 1)
              == QStringLiteral("http://127.0.0.1:7890"));
        CHECK(args.at(args.indexOf(QStringLiteral("-sdcard")) + 1) == QStringLiteral("/tmp/sd.img"));
        CHECK(args.at(args.indexOf(QStringLiteral("-netsim-args")) + 1)
              == QStringLiteral("--wifi=ChinaNet-abcd"));
        // -qemu -m 1536M 必须在末尾（-qemu 后的参数原样交给 QEMU）
        CHECK(args.size() >= 3);
        CHECK(args.at(args.size() - 3) == QStringLiteral("-qemu"));
        CHECK(args.at(args.size() - 2) == QStringLiteral("-m"));
        CHECK(args.at(args.size() - 1) == QStringLiteral("1536M"));
        // extraArgs 在 -sdcard 之后、-netsim-args 之前
        CHECK(args.indexOf(QStringLiteral("-cores")) > args.indexOf(QStringLiteral("-sdcard")));
        CHECK(args.indexOf(QStringLiteral("-cores")) < args.indexOf(QStringLiteral("-netsim-args")));
    }
    // 自动探测快照传入（resolvedSnapshot 由 launchHeadless 解析）
    {
        LaunchOptions o;
        const QStringList args =
            buildEmuArgs(QStringLiteral("avd1"), o, QStringLiteral("mwine_default"));
        CHECK(args.at(args.indexOf(QStringLiteral("-snapshot")) + 1)
              == QStringLiteral("mwine_default"));
    }
    // 冷启动：无 -snapshot，有 -no-snapshot
    {
        LaunchOptions o;
        o.noSnapshot = true;
        const QStringList args =
            buildEmuArgs(QStringLiteral("avd1"), o, QStringLiteral("ignored"));
        CHECK(!args.contains(QStringLiteral("-snapshot")));
        CHECK(args.contains(QStringLiteral("-no-snapshot")));
        CHECK(args.contains(QStringLiteral("-avd")));
    }
    // 自建镜像模式：忽略 AVD，-sysdir 直载；vendor 缺失不传
    {
        QTemporaryDir d;
        const QString img = d.path();
        for (const char* f : { "system-qemu.img", "ramdisk-qemu.img", "kernel-ranchu" }) {
            QFile file(QDir(img).filePath(QString::fromLatin1(f)));
            (void)file.open(QIODevice::WriteOnly);
            file.write(QByteArray(4 * 1024 * 1024, '\0'));
            file.close();
        }
        LaunchOptions o;
        o.imageDir = img;
        o.windowed = true;
        const QStringList args = buildEmuArgs(QStringLiteral("whatever"), o, std::nullopt);
        CHECK(!args.contains(QStringLiteral("-avd")));
        CHECK(args.at(args.indexOf(QStringLiteral("-sysdir")) + 1) == img);
        CHECK(args.at(args.indexOf(QStringLiteral("-system")) + 1)
              == QDir(img).filePath(QStringLiteral("system-qemu.img")));
        CHECK(args.at(args.indexOf(QStringLiteral("-ramdisk")) + 1)
              == QDir(img).filePath(QStringLiteral("ramdisk-qemu.img")));
        CHECK(args.at(args.indexOf(QStringLiteral("-kernel")) + 1)
              == QDir(img).filePath(QStringLiteral("kernel-ranchu")));
        CHECK(!args.contains(QStringLiteral("-vendor")));
        CHECK(args.at(args.indexOf(QStringLiteral("-datadir")) + 1)
              == QStringLiteral("/tmp/mwine-custom-data"));
        CHECK(!args.contains(QStringLiteral("-no-window"))); // windowed
        // 自定义 datadir 覆盖默认
        o.dataDir = QStringLiteral("/data/persist");
        const QStringList args2 = buildEmuArgs(QStringLiteral("whatever"), o, std::nullopt);
        CHECK(args2.at(args2.indexOf(QStringLiteral("-datadir")) + 1)
              == QStringLiteral("/data/persist"));
    }
}

static void testBuildEmuEnv()
{
    const Sdk sdk = Sdk::fromRoot(QStringLiteral("/tmp/mwine-fake-sdk"));
    LaunchOptions o;
    o.avdHome = QStringLiteral("/custom/avd");
    const auto env = buildEmuEnv(sdk, o, 5038);
    CHECK(env.value(QStringLiteral("ANDROID_SDK_ROOT")) == QStringLiteral("/tmp/mwine-fake-sdk"));
    CHECK(env.value(QStringLiteral("ANDROID_ADB_SERVER_PORT")) == QStringLiteral("5038"));
    CHECK(env.value(QStringLiteral("ANDROID_AVD_HOME")) == QStringLiteral("/custom/avd"));
    CHECK(env.value(QStringLiteral("PATH"))
              .startsWith(QStringLiteral("/tmp/mwine-fake-sdk/platform-tools")));
    // 显式 avdHome 未设时非 bundled 不注入 ANDROID_AVD_HOME
    LaunchOptions o2;
    CHECK(buildEmuEnv(sdk, o2, 5037)
              .value(QStringLiteral("ANDROID_AVD_HOME"))
              .isEmpty());
}

static void testValidateImageDir()
{
    QTemporaryDir d;
    const QString img = d.path();
    CHECK(!validateImageDir(img)); // 空目录
    {
        QFile f(QDir(img).filePath(QStringLiteral("system-qemu.img")));
        (void)f.open(QIODevice::WriteOnly);
        f.write(QByteArray(1024, '\0'));
        f.close();
    }
    CHECK(!validateImageDir(img)); // 缺 ramdisk/kernel
    QFile rk(QDir(img).filePath(QStringLiteral("kernel-ranchu")));
    (void)rk.open(QIODevice::WriteOnly);
    rk.write(QByteArray(64, '\0'));
    rk.close();
    QFile rd(QDir(img).filePath(QStringLiteral("ramdisk-qemu.img")));
    (void)rd.open(QIODevice::WriteOnly);
    rd.write(QByteArray(1 * 1024 * 1024, '\0')); // 仅 1MB：构建不完整
    rd.close();
    CHECK(!validateImageDir(img));
    QFile::remove(rd.fileName());
    (void)rd.open(QIODevice::WriteOnly);
    rd.write(QByteArray(4 * 1024 * 1024, '\0')); // ≥3MB
    rd.close();
    CHECK(validateImageDir(img));
}

// ===================== 快照列表 =====================

static void setMtime(const QString& path, qint64 secs)
{
    struct timespec ts[2];
    ts[0].tv_sec = secs;
    ts[0].tv_nsec = 0;
    ts[1] = ts[0];
    ::utimensat(AT_FDCWD, QFile::encodeName(path).constData(), ts, 0);
}

static void testAvdSnapshotNames()
{
    QTemporaryDir d;
    const QString avdDir = d.path();
    const QDir snaps(QDir(avdDir).filePath(QStringLiteral("snapshots")));
    for (const char* n : { "older", "newer", "incomplete" }) {
        const QString dir = snaps.filePath(QString::fromLatin1(n));
        QDir().mkpath(dir);
        QFile pb(QDir(dir).filePath(QStringLiteral("snapshot.pb")));
        (void)pb.open(QIODevice::WriteOnly);
        pb.write("x");
        pb.close();
        if (QLatin1String(n) == QLatin1String("incomplete"))
            QFile::remove(pb.fileName()); // 无 snapshot.pb = 不完整，不列出
    }
    setMtime(snaps.filePath(QStringLiteral("older")), 1000000);
    setMtime(snaps.filePath(QStringLiteral("newer")), 2000000);
    setMtime(snaps.filePath(QStringLiteral("incomplete")), 500000); // latestSnapshot 不校验 snapshot.pb

    const auto list = avdSnapshotNames(avdDir);
    CHECK(list.size() == 2);
    if (list.size() == 2) {
        CHECK(list.at(0).first == QStringLiteral("newer")); // 新的在前
        CHECK(list.at(0).second == 2000000 * 1000);          // mtime 毫秒
        CHECK(list.at(1).first == QStringLiteral("older"));
    }
    // latest_snapshot 探测：无 mwine_default 时取最新
    AvdInfo avd;
    avd.name = QStringLiteral("t");
    avd.path = avdDir;
    CHECK(latestSnapshot(avd) == QStringLiteral("newer"));
    // mwine_default 优先
    const QString def = snaps.filePath(QStringLiteral("mwine_default"));
    QDir().mkpath(def);
    QFile pb2(QDir(def).filePath(QStringLiteral("snapshot.pb")));
    (void)pb2.open(QIODevice::WriteOnly);
    pb2.write("x");
    pb2.close();
    CHECK(latestSnapshot(avd) == QStringLiteral("mwine_default"));
}

// ===================== 控制台端口 / 版本名 / properties / XML =====================

static void testMisc()
{
    // parse_properties（对齐 Rust parse_properties_basic）
    {
        const QString text = QStringLiteral(
            "# comment\nabi.type=arm64-v8a\nimage.sysdir.1=system-images/android-30/default/"
            "arm64-v8a/\nbroken line without equals\n");
        const auto map = parseProperties(text);
        CHECK(map.value(QStringLiteral("abi.type")) == QStringLiteral("arm64-v8a"));
        CHECK(map.value(QStringLiteral("image.sysdir.1"))
                  .contains(QStringLiteral("android-30")));
        CHECK(map.size() == 2);
    }
    // console port 解析
    {
        auto ok = consolePortFromSerial(QStringLiteral("emulator-5554"));
        CHECK(ok && ok.value() == 5554);
        CHECK(!consolePortFromSerial(QStringLiteral("localhost:5055")));
        CHECK(!consolePortFromSerial(QStringLiteral("emulator-abc")));
    }
    // Android 版本名
    CHECK(androidVersionName(QStringLiteral("android-33")) == QStringLiteral("Android 13"));
    CHECK(androidVersionName(QStringLiteral("android-36")) == QStringLiteral("Android 16"));
    CHECK(androidVersionName(QStringLiteral("android-32")) == QStringLiteral("Android 12L"));
    CHECK(androidVersionName(QStringLiteral("android-33-ext15")) == QStringLiteral("Android 13"));
    CHECK(!androidVersionName(QStringLiteral("android-99")).has_value());
    CHECK(!androidVersionName(QStringLiteral("wear")).has_value());
}

static void testParseRepoXml()
{
    const QString xml = QStringLiteral(
        "<ns:repository>"
        "<remotePackage path=\"system-images;android-33;default;arm64-v8a\">"
        "<display-name>Android 33</display-name>"
        "<url>sys-img-33.zip</url><size>12345678</size></remotePackage>"
        "<remotePackage path=\"system-images;android-30;default;x86_64\">"
        "<url>x86.zip</url><size>99</size></remotePackage>"
        "<remotePackage path=\"system-images;android-36;google_apis;arm64-v8a\">"
        "<url>sys-img-36.zip</url><size>42</size></remotePackage>"
        "<remotePackage path=\"platform-tools\"><url>pt.zip</url></remotePackage>"
        "</ns:repository>");
    auto r = parseRepoXml(xml, QString());
    CHECK(r);
    if (r) {
        const auto& list = r.value();
        CHECK(list.size() == 2); // x86_64 与非 system-images 排除
        CHECK(list.at(0).path
              == QStringLiteral("system-images;android-36;google_apis;arm64-v8a")); // 降序
        CHECK(list.at(0).url.endsWith(QStringLiteral("/sys-img-36.zip")));
        CHECK(list.at(0).size == 42);
        CHECK(list.at(0).display.contains(QStringLiteral("Android 16")));
        CHECK(list.at(1).path == QStringLiteral("system-images;android-33;default;arm64-v8a"));
        CHECK(list.at(1).size == 12345678);
    }
    // 过滤：按版本名 / 按 api 段
    auto f1 = parseRepoXml(xml, QStringLiteral("Android 13"));
    CHECK(f1 && f1.value().size() == 1
          && f1.value().at(0).path.contains(QStringLiteral("android-33")));
    auto f2 = parseRepoXml(xml, QStringLiteral("android-36"));
    CHECK(f2 && f2.value().size() == 1
          && f2.value().at(0).path.contains(QStringLiteral("android-36")));
    auto f3 = parseRepoXml(xml, QStringLiteral("android-99"));
    CHECK(f3 && f3.value().isEmpty());
}

// ===================== sha256 / 稀疏化 =====================

static void testHashAndSparse()
{
    std::atomic_bool cancel{ false };
    // 已知向量
    {
        QTemporaryDir d;
        const QString p = QDir(d.path()).filePath(QStringLiteral("abc"));
        QFile f(p);
        (void)f.open(QIODevice::WriteOnly);
        f.write("abc");
        f.close();
        auto h = sha256File(p, cancel);
        CHECK(h);
        CHECK(h.value() == QByteArray("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    }
    // 取消点
    {
        QTemporaryDir d;
        const QString p = QDir(d.path()).filePath(QStringLiteral("x"));
        QFile f(p);
        (void)f.open(QIODevice::WriteOnly);
        f.write(QByteArray(16 * 1024 * 1024, '\1')); // 16MB：跨多个 4MiB 块
        f.close();
        cancel = true;
        auto h = sha256File(p, cancel);
        CHECK(!h);
        CHECK(h.error().contains(QStringLiteral("取消")));
        cancel = false;
        auto h2 = sha256File(p, cancel);
        CHECK(h2 && h2.value().size() == 64);
    }
    // 稀疏化：内容不变
    {
        QTemporaryDir d;
        const QString p = QDir(d.path()).filePath(QStringLiteral("img"));
        QFile f(p);
        (void)f.open(QIODevice::WriteOnly);
        f.write(QByteArray(2 * 1024 * 1024, '\x7f')); // 前段数据
        f.write(QByteArray(5 * 1024 * 1024, '\0'));   // 中段全零
        f.write(QByteArray(2 * 1024 * 1024, '\x33')); // 后段数据
        f.close();
        const QByteArray before = [&] {
            QFile r(p);
            (void)r.open(QIODevice::ReadOnly);
            const QByteArray b = r.readAll();
            r.close();
            return b;
        }();
        auto r = sparseRewriteFile(p, cancel);
        CHECK(r);
        QFile r2(p);
        (void)r2.open(QIODevice::ReadOnly);
        const QByteArray after = r2.readAll();
        r2.close();
        CHECK(after == before);
        CHECK(after.size() == 9 * 1024 * 1024);
    }
}

// ===================== Sdk::fromRoot 派生路径 =====================

static void testSdkFromRoot()
{
    const Sdk s = Sdk::fromRoot(QStringLiteral("/opt/sdk"));
    CHECK(s.emulatorBin == QStringLiteral("/opt/sdk/emulator/emulator"));
    CHECK(s.adbBin == QStringLiteral("/opt/sdk/platform-tools/adb"));
    CHECK(s.platformTools == QStringLiteral("/opt/sdk/platform-tools"));
    CHECK(s.systemImages == QStringLiteral("/opt/sdk/system-images"));
    CHECK(!s.isBundled());
    const Sdk b = Sdk::fromRoot(QStringLiteral("/Apps/MWine.app/Contents/Resources/runtime"));
    CHECK(b.isBundled());
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("mw_supervisor_test"));

    testTarRoundtrip();
    testTarRejects();
    testZipRoundtrip();
    testApplyDeviceMode();
    testSetLcd();
    testBuildEmuArgs();
    testBuildEmuEnv();
    testValidateImageDir();
    testAvdSnapshotNames();
    testMisc();
    testParseRepoXml();
    testHashAndSparse();
    testSdkFromRoot();

    if (g_failed == 0)
        qInfo().noquote() << QStringLiteral("mw_supervisor_test: 全部通过");
    else
        qWarning().noquote() << QStringLiteral("mw_supervisor_test: %1 项失败").arg(g_failed);
    return g_failed == 0 ? 0 : 1;
}
