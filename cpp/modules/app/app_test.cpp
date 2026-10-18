// mw_app 模块自测：zip reader / AXML / badging / dumpsys / manager（脚本 shell）/
// mock / 壳工具纯函数 / parseApk 端到端。全部离线——构造内存最小 zip 与 AXML，
// 不依赖真 APK、设备与外部工具。
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <functional>

#include "app.hpp"

using namespace mw::app;

static int g_failed = 0;
#define CHECK(cond)                                                                \
    do {                                                                           \
        if (!(cond)) {                                                             \
            ++g_failed;                                                            \
            qWarning("FAIL %s:%d  %s", __FILE__, __LINE__, #cond);                 \
        }                                                                          \
    } while (false)

#define CHECK_EQ(a, b) CHECK((a) == (b))

// ---------- 字节构造辅助 ----------
static void put16(QByteArray& b, quint16 v)
{
    b.append(char(quint8(v & 0xff)));
    b.append(char(quint8(v >> 8)));
}
static void put32(QByteArray& b, quint32 v)
{
    for (int i = 0; i < 4; ++i)
        b.append(char(quint8((v >> (8 * i)) & 0xff)));
}

// ---------- zip 构造（apk.rs 测试的手工最小 zip：stored + deflate）----------
struct ZipEntrySpec {
    QString name;
    QByteArray stored;   // 已按 method 编码的字节
    quint32 uncompLen;
    quint16 method;
};

static QByteArray buildTestZip(const QVector<ZipEntrySpec>& entries)
{
    QByteArray out, central;
    QVector<quint32> offsets;
    for (const auto& e : entries) {
        offsets.push_back(quint32(out.size()));
        put32(out, 0x04034b50);                    // LOC sig
        put16(out, 0);                             // ver need
        put16(out, 0);                             // flags
        put16(out, e.method);
        put32(out, 0);                             // time/date
        put32(out, 0);                             // crc（读取器不校验）
        put32(out, quint32(e.stored.size()));      // comp
        put32(out, e.uncompLen);                   // uncomp
        put16(out, quint16(e.name.toUtf8().size()));
        put16(out, 0);                             // extra len
        out.append(e.name.toUtf8());
        out.append(e.stored);
    }
    const quint32 cdOff = quint32(out.size());
    for (int i = 0; i < entries.size(); ++i) {
        const auto& e = entries[i];
        put32(central, 0x02014b50);                // CEN sig
        central.append(QByteArray(6, '\0'));       // verMade/verNeed/flags
        put16(central, e.method);
        put32(central, 0);                         // time/date
        put32(central, 0);                         // crc
        put32(central, quint32(e.stored.size()));  // comp
        put32(central, e.uncompLen);               // uncomp
        put16(central, quint16(e.name.toUtf8().size()));
        put16(central, 0);                         // extra
        put16(central, 0);                         // comment
        put16(central, 0);                         // disk start
        put16(central, 0);                         // int attr
        put32(central, 0);                         // ext attr
        put32(central, offsets[i]);
        central.append(e.name.toUtf8());
    }
    out.append(central);
    const quint16 total = quint16(entries.size());
    put32(out, 0x06054b50);                        // EOCD sig
    out.append(QByteArray(4, '\0'));               // disk numbers
    put16(out, total);
    put16(out, total);
    put32(out, quint32(central.size()));
    put32(out, cdOff);
    put16(out, 0);                                 // comment len
    return out;
}

static void testZipReader()
{
    QByteArray repetitive(5000, 'x');
    const auto deflated = rawCompress(repetitive);
    CHECK(deflated.ok());
    const auto entries = QVector<ZipEntrySpec>{
        {QStringLiteral("AndroidManifest.xml"), QByteArray("<manifest/>"), 12, 0},
        {QStringLiteral("data.bin"), deflated.value(), quint32(repetitive.size()), 8},
    };
    const QByteArray bytes = buildTestZip(entries);

    auto zip = ZipArchive::fromBytes(bytes, QStringLiteral("test.apk"));
    CHECK(zip.ok());
    auto manifest = zip.value().readEntry(QStringLiteral("AndroidManifest.xml"));
    CHECK(manifest.ok() && manifest.value().has_value());
    CHECK_EQ(manifest.value().value(), QByteArray("<manifest/>"));
    auto data = zip.value().readEntry(QStringLiteral("data.bin"));
    CHECK(data.ok() && data.value().has_value());
    CHECK_EQ(data.value()->size(), 5000);
    CHECK(data.value()->endsWith("xxxx"));

    // rawDeflate 与 rawCompress 互逆（截断流报错）
    CHECK_EQ(rawDeflate(deflated.value()).value(), repetitive);
    CHECK(!rawDeflate(deflated.value().left(5)).ok());

    // 缺 entry → nullopt；entry 名单
    auto missing = zip.value().readEntry(QStringLiteral("nope.bin"));
    CHECK(missing.ok() && !missing.value().has_value());
    CHECK(!zip.value().hasEntry(QStringLiteral("nope.bin")));
    CHECK_EQ(zip.value().entryNames(),
             (QStringList{QStringLiteral("AndroidManifest.xml"), QStringLiteral("data.bin")}));

    // 垃圾数据
    auto bad = ZipArchive::fromBytes(QByteArrayLiteral("\x01\x02\x03"), QStringLiteral("bad.apk"));
    CHECK(bad.isErr());
    CHECK(bad.error().contains(QStringLiteral("APK 解析失败")));
}

// ---------- AXML 构造（manifest.rs 测试的最小 AXML）----------
struct Attr {
    QString name;
    quint8 type;
    quint32 data;
};
struct TNode {
    QString name;
    QVector<Attr> attrs;
    QVector<TNode> children;
};

static QByteArray buildAxml(const QStringList& strings, const TNode& root)
{
    constexpr quint16 kStart = 0x0102;
    constexpr quint16 kEnd = 0x0103;
    constexpr quint32 kNoIndex = 0xFFFFFFFFu;
    constexpr quint32 kUtf8Flag = 1u << 8;

    QByteArray pool;
    QVector<quint32> offsets;
    for (const QString& s : strings) {
        offsets.push_back(quint32(pool.size()));
        const QByteArray bytes = s.toUtf8();
        pool.append(char(quint8(bytes.size()))); // utf16 len（<128 单字节）
        pool.append(char(quint8(bytes.size()))); // utf8 len
        pool.append(bytes);
        pool.append('\0');
        while (pool.size() % 4 != 0)
            pool.append('\0');
    }
    const auto idx = [&strings](const QString& s) -> quint32 {
        return quint32(strings.indexOf(s));
    };
    // AXML chunk 是平铺事件序列：START 的 size 只覆盖自身（不含子元素）
    //（注意：Qt 把 emit 定义为宏，此处命名为 emitNode）
    std::function<void(const TNode&, QByteArray&)> emitNode =
        [&](const TNode& node, QByteArray& buf) {
            const quint32 elIdx = idx(node.name);
            QByteArray start;
            put16(start, kStart);
            put16(start, 16); // headerSize
            const int sizePos = start.size();
            put32(start, 0);  // size 回填
            put32(start, 1);  // lineNumber
            put32(start, kNoIndex); // comment
            put32(start, kNoIndex); // ns
            put32(start, elIdx);
            put16(start, 0x14); // attrStart
            put16(start, 0x14); // attrSize
            put16(start, quint16(node.attrs.size()));
            put16(start, 0);    // idIndex
            put16(start, 0);    // classIndex
            put16(start, 0);    // styleIndex
            for (const Attr& a : node.attrs) {
                put32(start, kNoIndex); // attr ns
                put32(start, idx(a.name));
                put32(start, kNoIndex); // rawValue
                put16(start, 8);        // value size
                start.append('\0');     // res0
                start.append(char(quint8(a.type)));
                put32(start, a.data);
            }
            QByteArray end;
            put16(end, kEnd);
            put16(end, 16);
            put32(end, 24);
            put32(end, 1);              // lineNumber
            put32(end, kNoIndex);       // comment
            put32(end, kNoIndex);       // ns
            put32(end, elIdx);
            const quint32 size = quint32(start.size());
            memcpy(start.data() + sizePos, &size, 4); // LE 平台

            QByteArray childrenBuf;
            for (const TNode& c : node.children)
                emitNode(c, childrenBuf);
            buf.append(start);
            buf.append(childrenBuf);
            buf.append(end);
        };

    QByteArray elementsBuf;
    emitNode(root, elementsBuf);

    QByteArray out;
    put16(out, 0x0003); // RES_XML
    put16(out, 8);      // headerSize
    const int fileSizePos = out.size();
    put32(out, 0);      // 回填

    const int spStart = out.size();
    put16(out, 0x0001); // RES_STRING_POOL_TYPE
    put16(out, 28);     // headerSize
    const int spSizePos = out.size();
    put32(out, 0);      // 回填
    put32(out, quint32(strings.size()));
    put32(out, 0);      // style count
    put32(out, kUtf8Flag);
    put32(out, 0);      // stringsStart 回填
    put32(out, 0);      // stylesStart
    for (const quint32 off : offsets)
        put32(out, off);
    out.append(pool);
    const quint32 spSize = quint32(out.size() - spStart);
    memcpy(out.data() + spSizePos, &spSize, 4);
    const quint32 stringsStart = 28 + quint32(strings.size()) * 4;
    memcpy(out.data() + spStart + 20, &stringsStart, 4);

    out.append(elementsBuf);
    const quint32 total = quint32(out.size());
    memcpy(out.data() + fileSizePos, &total, 4);
    return out;
}

static void testManifest()
{
    constexpr quint8 kTypeString = 0x03;
    constexpr quint8 kTypeIntDec = 0x10;

    {
        const QStringList strings = {QStringLiteral("manifest"),  QStringLiteral("package"),
                                     QStringLiteral("com.example.app"),
                                     QStringLiteral("versionName"), QStringLiteral("1.2.3"),
                                     QStringLiteral("application"), QStringLiteral("label"),
                                     QStringLiteral("示例应用"),     QStringLiteral("versionCode")};
        const TNode application{QStringLiteral("application"),
                                {Attr{QStringLiteral("label"), kTypeString, 7}},
                                {}};
        const TNode manifest{QStringLiteral("manifest"),
                             {Attr{QStringLiteral("package"), kTypeString, 2},
                              Attr{QStringLiteral("versionCode"), kTypeIntDec, 42},
                              Attr{QStringLiteral("versionName"), kTypeString, 4}},
                             {application}};
        auto info = parseManifest(buildAxml(strings, manifest), QStringLiteral("test"));
        CHECK(info.ok());
        CHECK_EQ(info.value().package, QStringLiteral("com.example.app"));
        CHECK_EQ(info.value().versionCode, std::optional<quint64>(42));
        CHECK_EQ(info.value().versionName, std::optional<QString>(QStringLiteral("1.2.3")));
        CHECK_EQ(info.value().label, std::optional<QString>(QStringLiteral("示例应用")));
    }
    {
        // label 属性是引用类型（dataType=0x01）：无法解析，应得 nullopt 而非报错
        const QStringList strings = {QStringLiteral("manifest"), QStringLiteral("package"),
                                     QStringLiteral("com.ref.app"), QStringLiteral("application"),
                                     QStringLiteral("label")};
        const TNode application{QStringLiteral("application"),
                                {Attr{QStringLiteral("label"), 0x01, 0x7f0e0001}},
                                {}};
        const TNode manifest{QStringLiteral("manifest"),
                             {Attr{QStringLiteral("package"), kTypeString, 2}},
                             {application}};
        auto info = parseManifest(buildAxml(strings, manifest), QStringLiteral("test"));
        CHECK(info.ok());
        CHECK_EQ(info.value().package, QStringLiteral("com.ref.app"));
        CHECK(!info.value().label.has_value());
    }
    {
        // application 被嵌到更深一层（模拟意外结构）：label 不应被采信
        const QStringList strings = {
            QStringLiteral("manifest"), QStringLiteral("package"), QStringLiteral("com.deep.app"),
            QStringLiteral("middle"),   QStringLiteral("application"), QStringLiteral("label"),
            QStringLiteral("深层的")};
        const TNode application{QStringLiteral("application"),
                                {Attr{QStringLiteral("label"), kTypeString, 6}},
                                {}};
        const TNode middle{QStringLiteral("middle"), {}, {application}};
        const TNode manifest{QStringLiteral("manifest"),
                             {Attr{QStringLiteral("package"), kTypeString, 2}},
                             {middle}};
        auto info = parseManifest(buildAxml(strings, manifest), QStringLiteral("test"));
        CHECK(info.ok());
        CHECK_EQ(info.value().package, QStringLiteral("com.deep.app"));
        CHECK(!info.value().label.has_value());
    }
    // 文本 XML / 空 / 只有文件头
    CHECK(parseManifest(QByteArrayLiteral("<manifest/>"), QStringLiteral("t")).isErr());
    CHECK(parseManifest(QByteArray(), QStringLiteral("t")).isErr());
    QByteArray headerOnly;
    put16(headerOnly, 0x0003);
    put16(headerOnly, 8);
    put32(headerOnly, 8);
    CHECK(parseManifest(headerOnly, QStringLiteral("t")).isErr());
}

// ---------- badging ----------
static void testBadging()
{
    const QString sample = QStringLiteral(
        "package: name='com.example.app' versionCode='42' versionName='1.2.3' "
        "platformBuildVersionCode='34'\n"
        "sdkVersion:'24'\n"
        "targetSdkVersion:'34'\n"
        "application: label='示例应用' icon='res/mipmap-xxxhdpi-v4/ic_launcher.png'\n"
        "application-label:'示例应用'\n"
        "application-label-hi:'उदाहरण'\n"
        "application-icon-160:'res/mipmap-hdpi-v4/ic_launcher.png'\n"
        "application-icon-240:'res/mipmap-hdpi-v4/ic_launcher.png'\n"
        "application-icon-640:'res/mipmap-xxxhdpi-v4/ic_launcher.png'\n"
        "launchable-activity: name='com.example.app.MainActivity'\n");
    {
        const ApkBadging b = parseBadging(sample);
        CHECK_EQ(b.package, QStringLiteral("com.example.app"));
        CHECK_EQ(b.label, std::optional<QString>(QStringLiteral("示例应用")));
        CHECK_EQ(b.icon,
                 std::optional<QString>(QStringLiteral("res/mipmap-xxxhdpi-v4/ic_launcher.png")));
    }
    {
        // 全 xml 池保留 xml；混有 png 时优先 png
        const QString text = QStringLiteral(
            "package: name='com.adaptive' versionName='1'\n"
            "application-icon-480:'res/mipmap-xxhdpi/ic_launcher.xml'\n"
            "application-icon-160:'res/mipmap-mdpi-v4/ic_launcher.png'\n");
        const ApkBadging b = parseBadging(text);
        CHECK_EQ(b.icon,
                 std::optional<QString>(QStringLiteral("res/mipmap-mdpi-v4/ic_launcher.png")));
    }
    {
        const QStringList entries = {
            QStringLiteral("res/mipmap-mdpi-v4/ic_launcher.png"),
            QStringLiteral("res/mipmap-xxhdpi-v4/ic_launcher.png"),
            QStringLiteral("res/mipmap-xxhdpi-v4/other.png"),
            QStringLiteral("res/mipmap-anydpi-v26/ic_launcher.xml"),
        };
        CHECK_EQ(pickRasterFallback(
                     entries, QStringLiteral("res/mipmap-anydpi-v26/ic_launcher.xml")),
                 std::optional<QString>(
                     QStringLiteral("res/mipmap-xxhdpi-v4/ic_launcher.png")));
    }
    {
        const ApkBadging b = parseBadging(QString());
        CHECK_EQ(b.package, QString());
        CHECK(!b.icon.has_value());
    }
}

// ---------- dumpsys ----------
static void testDumpsys()
{
    const QString single = QStringLiteral(
        "Package [com.example.app] (0x123 /data/app/~~abc==/com.example.app-xyz==/base.apk)\n"
        "  userId=10123\n"
        "  pkg=Package{... com.example.app}\n"
        "  versionCode=42 minSdk=24 targetSdk=34\n"
        "  versionName=1.2.3\n"
        "  firstInstallTime=2026-10-01 12:00:00\n"
        "  lastUpdateTime=2026-10-02 13:00:00\n"
        "  installerPackageName=null\n"
        "  signatures=...");
    {
        const PackageDump d = parseDumpsysPackage(single);
        CHECK_EQ(d.versionName, std::optional<QString>(QStringLiteral("1.2.3")));
        CHECK_EQ(d.versionCode, std::optional<quint64>(42));
        CHECK_EQ(d.firstInstallTime,
                 std::optional<QString>(QStringLiteral("2026-10-01 12:00:00")));
        CHECK_EQ(d.lastUpdateTime,
                 std::optional<QString>(QStringLiteral("2026-10-02 13:00:00")));
    }
    {
        const QString other = QString(single)
                                  .replace(QLatin1String("com.example.app"),
                                           QLatin1String("com.other.app"))
                                  .replace(QLatin1String("1.2.3"), QLatin1String("9.9"));
        const auto map = parseDumpsysAll(single + QLatin1Char('\n') + other);
        CHECK_EQ(map.size(), 2);
        CHECK_EQ(map.value(QStringLiteral("com.example.app")).versionName,
                 std::optional<QString>(QStringLiteral("1.2.3")));
        CHECK_EQ(map.value(QStringLiteral("com.other.app")).versionName,
                 std::optional<QString>(QStringLiteral("9.9")));
    }
    {
        const QString ps = QStringLiteral(
            "NAME\nsystem\ncom.example.app\ncom.example.app:service\ncom.other\n");
        const QSet<QString> procs = parseProcessNames(ps);
        CHECK(packageRunning(procs, QStringLiteral("com.example.app")));
        CHECK(!packageRunning(procs, QStringLiteral("com.notinstalled")));
        // 前缀相同但不同包（com.example.appx）不算运行
        const QSet<QString> procs2 =
            parseProcessNames(QStringLiteral("NAME\ncom.example.appx\n"));
        CHECK(!packageRunning(procs2, QStringLiteral("com.example.app")));
    }
    {
        const QString out = QStringLiteral(
            "priority=0 preferredOrder=0 match=0x00108000 specificIndex=-1 isDefault=true\n"
            "com.example.app/.MainActivity\n");
        CHECK_EQ(parseResolveActivity(out),
                 std::optional<QString>(QStringLiteral("com.example.app/.MainActivity")));
        // 无入口：全部行都含空格（如 "No activity found"）
        CHECK(!parseResolveActivity(QStringLiteral("No activity found\n")).has_value());
    }
}

// ---------- manager（脚本化 shell）----------
namespace {

class ScriptedShell : public DeviceShell {
public:
    explicit ScriptedShell(QVector<ShellReply> replies) : replies_(std::move(replies)) {}
    mw::Result<ShellReply> run(const QString& cmd, int) override
    {
        Q_UNUSED(cmd)
        if (idx_ >= replies_.size())
            return mw::err<ShellReply>(errShell(QStringLiteral("script exhausted"), QString()));
        return mw::Result<ShellReply>::Ok(replies_[idx_++]);
    }

private:
    QVector<ShellReply> replies_;
    int idx_ = 0;
};

ShellReply okReply(const QString& out) { return ShellReply{out, true}; }

std::unique_ptr<AdbAppManager> managerWith(QVector<ShellReply> replies)
{
    return std::make_unique<AdbAppManager>(
        std::make_unique<ScriptedShell>(std::move(replies)), QStringLiteral("emulator-test"));
}

} // namespace

static void testManager()
{
    {
        // list fast + enrich（dumpsys 全量 + ps）
        auto m = managerWith({
            okReply(QStringLiteral("package:com.a.app\npackage:com.b.app\n")),
            okReply(QStringLiteral(
                "Package [com.a.app]\n  versionCode=10 minSdk=24\n  versionName=1.0\n  "
                "firstInstallTime=2026-10-01 10:00:00\n  lastUpdateTime=2026-10-01 10:00:00\n\n"
                "Package [com.b.app]\n  versionName=2.0\n")),
            okReply(QStringLiteral("NAME\ncom.a.app\ncom.a.app:svc\n")),
        });
        ListOptions opts;
        opts.enrich = true;
        auto r = m->list(opts);
        CHECK(r.ok());
        const auto& apps = r.value();
        CHECK_EQ(apps.size(), 2);
        CHECK_EQ(apps[0].package, QStringLiteral("com.a.app"));
        CHECK_EQ(apps[0].version, std::optional<QString>(QStringLiteral("1.0")));
        CHECK_EQ(apps[0].versionCode, std::optional<quint64>(10));
        CHECK(apps[0].isRunning);
        CHECK_EQ(apps[0].installedAt,
                 std::optional<QString>(QStringLiteral("2026-10-01 10:00:00")));
        CHECK(!apps[1].isRunning);
        CHECK_EQ(apps[1].version, std::optional<QString>(QStringLiteral("2.0")));
    }
    {
        // launch：resolve-activity → am start --display
        auto m = managerWith({
            okReply(QStringLiteral("package:com.a.app\n")),
            okReply(QStringLiteral("com.a.app/.MainActivity\n")),
            okReply(QStringLiteral("Starting: Intent { cmp=com.a.app/.MainActivity }\n")),
        });
        CHECK(m->launch(QStringLiteral("com.a.app"), 3).ok());
    }
    {
        // launch：resolve 失败回退 monkey
        auto m = managerWith({
            okReply(QStringLiteral("package:com.a.app\n")),
            okReply(QStringLiteral("No activity found\n")),
            okReply(QStringLiteral("Events injected: 1\n## Network stats: elapsed=...\n")),
        });
        CHECK(m->launch(QStringLiteral("com.a.app"), std::nullopt).ok());
    }
    {
        auto m = managerWith({okReply(QString())});
        auto err = m->launch(QStringLiteral("com.nothere"), std::nullopt);
        CHECK(err.isErr());
        CHECK(err.error().contains(QStringLiteral("未安装")));
    }
    {
        // uninstall：按输出判定 Success
        auto m = managerWith({
            okReply(QStringLiteral("package:com.a.app\n")),
            okReply(QStringLiteral("Success\n")),
        });
        CHECK(m->uninstall(QStringLiteral("com.a.app")).ok());

        auto m2 = managerWith({
            okReply(QStringLiteral("package:com.a.app\n")),
            okReply(QStringLiteral("Failure [DELETE_FAILED_INTERNAL_ERROR]\n")),
        });
        CHECK(m2->uninstall(QStringLiteral("com.a.app")).isErr());
    }
    {
        // is_installed：仅精确整行匹配（前缀包并存不影响）
        auto m = managerWith({
            okReply(QStringLiteral("package:com.a.app\npackage:com.a.appx\n")),
        });
        CHECK(m->isInstalled(QStringLiteral("com.a.appx")).value().value);

        auto m2 = managerWith({okReply(QStringLiteral("package:com.a.appx\n"))});
        CHECK(!m2->isInstalled(QStringLiteral("com.a.app")).value().value);
    }
    {
        // 启发式图标：res/ 下名字含 launcher/icon 的最高密度位图
        const QStringList entries = {
            QStringLiteral("res/mipmap-mdpi-v4/ic_launcher.png"),
            QStringLiteral("res/mipmap-xxhdpi-v4/ic_launcher.png"),
            QStringLiteral("res/drawable/splash.png"),
            QStringLiteral("res/mipmap-anydpi-v26/ic_launcher.xml"),
        };
        CHECK_EQ(AdbAppManager::heuristicIcon(entries),
                 std::optional<QString>(
                     QStringLiteral("res/mipmap-xxhdpi-v4/ic_launcher.png")));
        CHECK(!AdbAppManager::heuristicIcon({}).has_value());
    }
}

// ---------- mock ----------
static void testMock()
{
    {
        MockAppManager m;
        auto info = m.install(QStringLiteral("/tmp/demo.apk"));
        CHECK(info.ok());
        CHECK_EQ(info.value().package, QStringLiteral("com.mock.demo"));
        CHECK(m.isInstalled(QStringLiteral("com.mock.demo")).value().value);

        CHECK(m.launch(QStringLiteral("com.mock.demo"), 2).ok());
        CHECK_EQ(m.launchLog.size(), 1);
        CHECK_EQ(m.launchLog[0].first, QStringLiteral("com.mock.demo"));
        CHECK_EQ(m.launchLog[0].second, std::optional<quint32>(2));
        ListOptions opts;
        opts.enrich = true;
        const auto apps = m.list(opts);
        CHECK(apps.ok() && apps.value()[0].isRunning);

        CHECK(m.stop(QStringLiteral("com.mock.demo")).ok());
        CHECK(m.uninstall(QStringLiteral("com.mock.demo")).ok());
        CHECK(!m.isInstalled(QStringLiteral("com.mock.demo")).value().value);
        CHECK(m.get(QStringLiteral("com.mock.demo")).isErr());
    }
    {
        MockAppManager m;
        CHECK(m.launch(QStringLiteral("com.nothere"), std::nullopt).isErr());
        CHECK(m.uninstall(QStringLiteral("com.nothere")).isErr());
    }
    {
        MockAppManager m;
        m.withApp(QStringLiteral("com.a.app"), QStringLiteral("A"));
        m.failOn = QStringLiteral("com.a.app");
        CHECK(m.launch(QStringLiteral("com.a.app"), std::nullopt).isErr());
        CHECK(m.uninstall(QStringLiteral("com.a.app")).isErr());
    }
}

// ---------- 壳工具纯函数 ----------
static void testShellGen()
{
    CHECK_EQ(sanitizeFileName(QStringLiteral("a/b:c\\d\"e'f$g`h")),
             QStringLiteral("a_b_c_d_e_f_g_h"));
    CHECK_EQ(sanitizeFileName(QStringLiteral("  王者营地  ")), QStringLiteral("王者营地"));
    CHECK_EQ(xmlEscape(QStringLiteral("&<>\"'")),
             QStringLiteral("&amp;&lt;&gt;&quot;&apos;"));

    {
        const QByteArray conf = QByteArrayLiteral(
            "package=com.foo.bar\n"
            "title=示例应用\n"
            "avd=\n"
            "display=970x2048/430\n"
            "fps=30\n"
            "fullscreen=1\n"
            "bogus=x\n");
        const auto cfg = parseAppConf(conf);
        CHECK(cfg.has_value());
        CHECK_EQ(cfg->package, QStringLiteral("com.foo.bar"));
        CHECK_EQ(cfg->title, std::optional<QString>(QStringLiteral("示例应用")));
        CHECK(!cfg->avd.has_value());
        CHECK_EQ(cfg->display, QStringLiteral("970x2048/430"));
        CHECK_EQ(cfg->fps, std::optional<float>(30.0f));
        CHECK(cfg->fullscreen);
        CHECK(!parseAppConf(QByteArrayLiteral("title=x")).has_value());
        // display 缺省值
        const auto minimal = parseAppConf(QByteArrayLiteral("package=p\n"));
        CHECK(minimal.has_value());
        CHECK_EQ(minimal->display, QStringLiteral("944x2048/384"));
    }
    {
        QTemporaryDir tmp;
        CHECK(tmp.isValid());
        const QString fooApp =
            tmp.filePath(QStringLiteral("Foo.app/Contents/Resources"));
        QDir().mkpath(fooApp);
        {
            QFile f(QDir(fooApp).filePath(QStringLiteral("app.conf")));
            CHECK(f.open(QIODevice::WriteOnly));
            f.write("package=com.foo.bar\ntitle=Foo\n");
        }
        {
            QFile f(QDir(fooApp + QStringLiteral("/.."))
                        .filePath(QStringLiteral("Info.plist")));
            CHECK(f.open(QIODevice::WriteOnly));
            f.write("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\">\n"
                    "<dict>\n  <key>CFBundleExecutable</key><string>foo-bin</string>\n"
                    "</dict>\n</plist>\n");
        }
        QDir().mkpath(tmp.filePath(QStringLiteral("Bar.app/Contents/MacOS")));

        auto shells = scanAppShells(tmp.path());
        CHECK_EQ(shells.size(), 2);
        std::optional<AppShellEntry> foo;
        std::optional<AppShellEntry> bar;
        for (const auto& s : shells) {
            if (s.appDir.endsWith(QLatin1String("Foo.app")))
                foo = s;
            if (s.appDir.endsWith(QLatin1String("Bar.app")))
                bar = s;
        }
        CHECK(foo.has_value() && bar.has_value());
        CHECK_EQ(foo->conf.package, QStringLiteral("com.foo.bar"));
        CHECK_EQ(foo->exeName, QStringLiteral("foo-bin"));
        CHECK(!foo->hasIcon);
        CHECK(bar->conf.package.isEmpty());

        QFile icns(QDir(fooApp).filePath(QStringLiteral("AppIcon.icns")));
        CHECK(icns.open(QIODevice::WriteOnly));
        icns.write("icns");
        icns.close();
        shells = scanAppShells(tmp.path());
        for (const auto& s : shells) {
            if (s.appDir.endsWith(QLatin1String("Foo.app")))
                CHECK(s.hasIcon);
        }
    }
    {
        QTemporaryDir tmp;
        CHECK(tmp.isValid());
        const QString jsonPath = tmp.filePath(QStringLiteral("appmodes.json"));
        QFile f(jsonPath);
        CHECK(f.open(QIODevice::WriteOnly));
        f.write("{\"com.a.app\": \"window\", \"com.b.app\": \"emulator\"}");
        f.close();
        CHECK(bundledAppWindowMode(QStringLiteral("com.a.app"), jsonPath));
        CHECK(!bundledAppWindowMode(QStringLiteral("com.b.app"), jsonPath));
        CHECK(!bundledAppWindowMode(QStringLiteral("com.c.app"), jsonPath));
    }
    {
        // 图标 MIME 探测
        CHECK_EQ(detectIconMime(QStringLiteral("a.png"), QByteArray()),
                 std::optional<IconMime>(IconMime::Png));
        CHECK_EQ(detectIconMime(QStringLiteral("a"),
                                QByteArrayLiteral("\x89\x50\x4e\x47\r\n")),
                 std::optional<IconMime>(IconMime::Png));
        CHECK_EQ(detectIconMime(QStringLiteral("a.webp"), QByteArray()),
                 std::optional<IconMime>(IconMime::Webp));
        QByteArray riff = QByteArrayLiteral("RIFF1234WEBPVP8 ");
        CHECK_EQ(detectIconMime(QStringLiteral("a"), riff),
                 std::optional<IconMime>(IconMime::Webp));
        CHECK(!detectIconMime(QStringLiteral("a"), QByteArrayLiteral("GIF89a")).has_value());
    }
    {
        // AppInfo 展示名退化
        AppInfo noLabel;
        noLabel.package = QStringLiteral("com.foo.bar");
        CHECK_EQ(noLabel.displayName(), QStringLiteral("bar"));
        AppInfo withLabel;
        withLabel.package = QStringLiteral("com.foo.bar");
        withLabel.name = QStringLiteral("  ");
        CHECK_EQ(withLabel.displayName(), QStringLiteral("bar"));
        withLabel.name = QStringLiteral("Foo");
        CHECK_EQ(withLabel.displayName(), QStringLiteral("Foo"));
    }
}

// ---------- parseApk 端到端（内存构造 zip + AXML，无真 APK）----------
static void testParseApk()
{
    constexpr quint8 kTypeString = 0x03;
    constexpr quint8 kTypeIntDec = 0x10;
    const QStringList strings = {QStringLiteral("manifest"),  QStringLiteral("package"),
                                 QStringLiteral("com.example.app"),
                                 QStringLiteral("versionName"), QStringLiteral("1.2.3"),
                                 QStringLiteral("application"), QStringLiteral("label"),
                                 QStringLiteral("示例应用"),     QStringLiteral("versionCode")};
    const TNode application{QStringLiteral("application"),
                            {Attr{QStringLiteral("label"), kTypeString, 7}},
                            {}};
    const TNode manifest{QStringLiteral("manifest"),
                         {Attr{QStringLiteral("package"), kTypeString, 2},
                          Attr{QStringLiteral("versionCode"), kTypeIntDec, 42},
                          Attr{QStringLiteral("versionName"), kTypeString, 4}},
                         {application}};
    const QByteArray axml = buildAxml(strings, manifest);

    // PNG 魔数开头的伪图标载荷
    QByteArray icon("\x89\x50\x4e\x47\r\n\x1a\n", 8);
    icon.append(QByteArray(64, 'i'));

    QTemporaryDir tmp;
    const QString apkPath = tmp.filePath(QStringLiteral("demo.apk"));
    const QByteArray zipBytes = buildTestZip({
        {QStringLiteral("AndroidManifest.xml"), axml, quint32(axml.size()), 0},
        {QStringLiteral("res/mipmap-xxxhdpi-v4/ic_launcher.png"), icon,
         quint32(icon.size()), 0},
        {QStringLiteral("classes.dex"), QByteArrayLiteral("dex"), 3, 0},
    });
    {
        QFile f(apkPath);
        CHECK(f.open(QIODevice::WriteOnly));
        f.write(zipBytes);
    }

    auto r = parseApk(apkPath);
    CHECK(r.ok());
    CHECK_EQ(r.value().package, QStringLiteral("com.example.app"));
    CHECK_EQ(r.value().label, QStringLiteral("示例应用"));
    CHECK_EQ(r.value().versionName, QStringLiteral("1.2.3"));
    CHECK_EQ(r.value().versionCode, quint64(42));
    CHECK(r.value().iconPng.has_value());
    CHECK_EQ(*r.value().iconPng, icon);
    CHECK_EQ(r.value().iconMime, QStringLiteral("image/png"));
    CHECK_EQ(r.value().displayName(), QStringLiteral("示例应用"));

    // 无 label 字面量（引用）→ label 空、displayName 退化为包名末段
    const QStringList refStrings = {QStringLiteral("manifest"), QStringLiteral("package"),
                                    QStringLiteral("com.ref.app"), QStringLiteral("application"),
                                    QStringLiteral("label")};
    const TNode refApp{QStringLiteral("application"),
                       {Attr{QStringLiteral("label"), 0x01, 0x7f0e0001}},
                       {}};
    const TNode refManifest{QStringLiteral("manifest"),
                            {Attr{QStringLiteral("package"), kTypeString, 2}},
                            {refApp}};
    const QString refApk = tmp.filePath(QStringLiteral("ref.apk"));
    {
        const QByteArray refAxml = buildAxml(refStrings, refManifest);
        QFile f(refApk);
        CHECK(f.open(QIODevice::WriteOnly));
        f.write(buildTestZip({{QStringLiteral("AndroidManifest.xml"), refAxml,
                               quint32(refAxml.size()), 0}}));
    }
    auto r2 = parseApk(refApk);
    CHECK(r2.ok());
    CHECK_EQ(r2.value().package, QStringLiteral("com.ref.app"));
    CHECK(r2.value().label.isEmpty());
    CHECK_EQ(r2.value().displayName(), QStringLiteral("app"));
    CHECK(!r2.value().iconPng.has_value());
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    testZipReader();
    testManifest();
    testBadging();
    testDumpsys();
    testManager();
    testMock();
    testShellGen();
    testParseApk();
    if (g_failed == 0)
        qInfo("[mw_app_test] 全部通过");
    else
        qWarning("[mw_app_test] %d 项失败", g_failed);
    return g_failed == 0 ? 0 : 1;
}
