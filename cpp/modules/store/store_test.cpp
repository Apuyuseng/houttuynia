// mw_store 离线单测 ← crates/mwine-store 各 rs 文件的 #[cfg(test)] 用例移植。
// 纯逻辑：镜像选择、HTML/URL 解析、豌豆荚页面抽取、F-Droid 索引解析、
// 模型 JSON roundtrip、内置 zip+AXML、取消令牌——不发真实网络请求。
#include <QByteArray>
#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QFile>
#include <QTemporaryFile>

#include <atomic>
#include <cstring>
#include <functional>

#include <zlib.h>

#include "apk_min.hpp"
#include "fdroid.hpp"
#include "html.hpp"
#include "../common/compress.hpp"
#include "internal.hpp"
#include "store.hpp"
#include "wandoujia.hpp"

static int g_failures = 0;
#define CHECK(cond)                                                \
    do {                                                           \
        if (!(cond)) {                                             \
            ++g_failures;                                          \
            qWarning("FAIL %s:%d  %s", __FILE__, __LINE__, #cond); \
        }                                                          \
    } while (0)

using namespace mw::store;

// ================= 测试脚手架：zip / AXML 构造（移植 Rust 单测的 builder） =================

namespace {

void appendU16(QByteArray& b, quint16 v)
{
    b += char(v & 0xFF);
    b += char((v >> 8) & 0xFF);
}
void appendU32(QByteArray& b, quint32 v)
{
    for (int i = 0; i < 4; ++i)
        b += char((v >> (8 * i)) & 0xFF);
}
void patchU32(QByteArray& b, int pos, quint32 v)
{
    for (int i = 0; i < 4; ++i)
        b[pos + i] = char((v >> (8 * i)) & 0xFF);
}

struct Attr {
    const char* name;
    quint8 dtype;
    quint32 data;
};
struct TNode {
    const char* name;
    QVector<Attr> attrs;
    QVector<TNode> children;
};

constexpr quint16 RES_STRING_POOL_TYPE = 0x0001;
constexpr quint16 RES_XML_START_ELEMENT_TYPE = 0x0102;
constexpr quint16 RES_XML_END_ELEMENT_TYPE = 0x0103;
constexpr quint32 UTF8_FLAG = 1u << 8;
constexpr quint32 NO_INDEX = 0xFFFFFFFFu;
constexpr quint8 TYPE_STRING = 0x03;
constexpr quint8 TYPE_INT_DEC = 0x10;

// 最小可解析 AXML：UTF-8 string pool + 元素树（START/END 事件序列）。
QByteArray buildAxml(const QStringList& strings, const TNode& root)
{
    QByteArray pool;
    QVector<quint32> offsets;
    for (const QString& s : strings) {
        offsets.push_back(static_cast<quint32>(pool.size()));
        const QByteArray bytes = s.toUtf8();
        pool += char(bytes.size());  // utf16 len（<128 单字节）
        pool += char(bytes.size());  // utf8 len
        pool += bytes;
        pool += '\0';
    }
    while (pool.size() % 4 != 0)
        pool += '\0';

    const auto idx = [&strings](const char* s) -> quint32 {
        return static_cast<quint32>(strings.indexOf(QString::fromUtf8(s)));
    };

    // START chunk 的 size 只覆盖自身（AXML 是平铺事件序列）
    std::function<void(const TNode&, QByteArray&)> emitNode =
        [&](const TNode& node, QByteArray& buf) {
            const quint32 elIdx = idx(node.name);
            QByteArray start;
            appendU16(start, RES_XML_START_ELEMENT_TYPE);
            appendU16(start, 16);        // headerSize
            const int sizePos = start.size();
            appendU32(start, 0);         // size 回填
            appendU32(start, 1);         // lineNumber
            appendU32(start, NO_INDEX);  // comment
            appendU32(start, NO_INDEX);  // ns
            appendU32(start, elIdx);     // name
            appendU16(start, 0x14);      // attrStart
            appendU16(start, 0x14);      // attrSize
            appendU16(start, static_cast<quint16>(node.attrs.size()));
            appendU16(start, 0);         // idIndex
            appendU16(start, 0);         // classIndex
            appendU16(start, 0);         // styleIndex
            for (const Attr& a : node.attrs) {
                appendU32(start, NO_INDEX);  // attr ns
                appendU32(start, idx(a.name));
                appendU32(start, NO_INDEX);  // rawValue
                appendU16(start, 8);         // value size
                start += '\0';               // res0
                start += char(a.dtype);
                appendU32(start, a.data);
            }
            patchU32(start, sizePos, static_cast<quint32>(start.size()));  // 回填 size
            buf += start;
            for (const TNode& child : node.children)
                emitNode(child, buf);
            QByteArray end;
            appendU16(end, RES_XML_END_ELEMENT_TYPE);
            appendU16(end, 16);
            appendU32(end, 24);
            appendU32(end, 1);
            appendU32(end, NO_INDEX);
            appendU32(end, NO_INDEX);
            appendU32(end, elIdx);
            buf += end;
        };

    QByteArray elements;
    emitNode(root, elements);

    QByteArray out;
    appendU16(out, 0x0003);  // RES_XML
    appendU16(out, 8);       // headerSize
    const int fileSizePos = out.size();
    appendU32(out, 0);       // 回填

    const int spStart = out.size();
    appendU16(out, RES_STRING_POOL_TYPE);
    appendU16(out, 28);  // headerSize
    const int spSizePos = out.size();
    appendU32(out, 0);   // 回填
    appendU32(out, static_cast<quint32>(strings.size()));
    appendU32(out, 0);   // style count
    appendU32(out, UTF8_FLAG);
    const int stringsStartPos = out.size();
    appendU32(out, 0);   // stringsStart 回填
    appendU32(out, 0);   // stylesStart
    for (const quint32 off : offsets)
        appendU32(out, off);
    out += pool;
    patchU32(out, spSizePos, static_cast<quint32>(out.size() - spStart));
    patchU32(out, stringsStartPos, 28 + static_cast<quint32>(strings.size()) * 4);
    out += elements;
    patchU32(out, fileSizePos, static_cast<quint32>(out.size()));
    return out;
}

QByteArray deflateBytes(const QByteArray& in)
{
    z_stream zs{};
    deflateInit2(&zs, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.constData()));
    zs.avail_in = static_cast<uInt>(in.size());
    QByteArray out;
    char buf[65536];
    int ret = Z_OK;
    do {
        zs.next_out = reinterpret_cast<Bytef*>(buf);
        zs.avail_out = sizeof(buf);
        ret = deflate(&zs, Z_FINISH);
        out.append(buf, sizeof(buf) - zs.avail_out);
    } while (ret != Z_STREAM_END);
    deflateEnd(&zs);
    return out;
}

// 手工构造最小 zip：一个 store（AndroidManifest.xml）+ 一个 deflate entry。
QByteArray buildTestZip(const QByteArray& manifestAxml)
{
    struct Entry {
        const char* name;
        QByteArray stored;
        quint32 uncompLen;
        quint16 method;
    };
    QByteArray repetitive(5000, 'x');
    const Entry entries[] = {
        { "AndroidManifest.xml", manifestAxml, static_cast<quint32>(manifestAxml.size()), 0 },
        { "data.bin", deflateBytes(repetitive), static_cast<quint32>(repetitive.size()), 8 },
    };
    QByteArray out;
    QByteArray central;
    QVector<quint32> offsets;
    for (const Entry& e : entries) {
        offsets.push_back(static_cast<quint32>(out.size()));
        appendU32(out, 0x04034b50);  // LOC sig
        appendU16(out, 0);           // ver need
        appendU16(out, 0);           // flags
        appendU16(out, e.method);
        appendU32(out, 0);           // time/date
        appendU32(out, 0);           // crc（读取器不校验）
        appendU32(out, static_cast<quint32>(e.stored.size()));
        appendU32(out, e.uncompLen);
        appendU16(out, static_cast<quint16>(std::strlen(e.name)));
        appendU16(out, 0);           // extra len
        out += e.name;
        out += e.stored;
    }
    const quint32 cdOff = static_cast<quint32>(out.size());
    for (const Entry& e : entries) {
        const int i = static_cast<int>(&e - entries);
        appendU32(central, 0x02014b50);  // CEN sig
        central.append(6, '\0');         // verMade/verNeed/flags
        appendU16(central, e.method);
        appendU32(central, 0);           // time/date
        appendU32(central, 0);           // crc
        appendU32(central, static_cast<quint32>(e.stored.size()));
        appendU32(central, e.uncompLen);
        appendU16(central, static_cast<quint16>(std::strlen(e.name)));
        appendU16(central, 0);           // extra
        appendU16(central, 0);           // comment
        appendU16(central, 0);           // disk start
        appendU16(central, 0);           // int attr
        appendU32(central, 0);           // ext attr
        appendU32(central, offsets.at(i));
        central += e.name;
    }
    out += central;
    const quint16 total = static_cast<quint16>(sizeof(entries) / sizeof(entries[0]));
    appendU32(out, 0x06054b50);  // EOCD sig
    out.append(4, '\0');         // disk numbers
    appendU16(out, total);
    appendU16(out, total);
    appendU32(out, static_cast<quint32>(central.size()));
    appendU32(out, cdOff);
    appendU16(out, 0);           // comment len
    return out;
}

// 造好的 APK 写入临时文件（用后删）。
class TempApk {
public:
    explicit TempApk(const QByteArray& bytes)
    {
        QTemporaryFile tf;
        tf.setAutoRemove(false);
        (void)tf.open();
        tf.write(bytes);
        path_ = tf.fileName();
    }
    ~TempApk() { QFile::remove(path_); }
    const QString& path() const { return path_; }

private:
    QString path_;
};

}  // namespace

// ================= html =================

static void testHtml()
{
    const QString h =
        QStringLiteral("<a data-app-pname=\"com.tencent.mm\" data-app-vcode=\"3180\">x</a>");
    CHECK(html::attr(h, QStringLiteral("data-app-pname")) == QStringLiteral("com.tencent.mm"));
    CHECK(html::attr(h, QStringLiteral("data-app-vcode")) == QStringLiteral("3180"));
    CHECK(!html::attr(h, QStringLiteral("missing")).has_value());

    CHECK(html::decodeEntities(QStringLiteral("a&amp;b")) == QStringLiteral("a&b"));
    CHECK(html::decodeEntities(QStringLiteral("&#24494;&#20449;")) == QStringLiteral("微信"));
    CHECK(html::decodeEntities(QStringLiteral("a&#x26;b")) == QStringLiteral("a&b"));
    CHECK(html::decodeEntities(QStringLiteral("无实体")) == QStringLiteral("无实体"));
    CHECK(html::decodeEntities(QStringLiteral("残缺&abc")) == QStringLiteral("残缺&abc"));

    CHECK(html::stripTags(QStringLiteral("<p>你好 <b>世界</b></p>")) == QStringLiteral("你好 世界"));

    CHECK(html::urlencode(QStringLiteral("微信")) == QStringLiteral("%E5%BE%AE%E4%BF%A1"));
    CHECK(html::urldecode(QStringLiteral("%E5%BE%AE%E4%BF%A1")) == QStringLiteral("微信"));
    CHECK(html::urldecode(QStringLiteral("a+b")) == QStringLiteral("a b"));

    const QString url = QStringLiteral(
        "https://x/binding?downloadUrl=https%3A%2F%2Fy.apk%3Fmd5%3Dabc%26size%3D10&ch=1");
    CHECK(html::queryParam(url, QStringLiteral("downloadUrl"))
          == QStringLiteral("https://y.apk?md5=abc&size=10"));
    CHECK(html::queryParam(url, QStringLiteral("ch")) == QStringLiteral("1"));
    CHECK(!html::queryParam(url, QStringLiteral("missing")).has_value());

    CHECK(html::between(QStringLiteral("<div class=\"comment\"> hi </div>"),
                        QStringLiteral("comment\">"), QStringLiteral("</div>"))
          == QStringLiteral(" hi "));

    const QString nth = QStringLiteral("<a data-app-vcode=\"1\"/><b/><a data-app-vcode=\"2\"/>");
    CHECK(html::attrNth(nth, QStringLiteral("data-app-vcode"), 0) == QStringLiteral("1"));
    CHECK(html::attrNth(nth, QStringLiteral("data-app-vcode"), 1) == QStringLiteral("2"));
    CHECK(!html::attrNth(nth, QStringLiteral("data-app-vcode"), 2).has_value());
}

// ================= 豌豆荚页面解析 =================

static const char* kSearchHtml = R"(
<ul id="j-search-list" class="app-box clearfix">
<li class="search-item search-searchitems">
    <div class="icon-wrap">
        <a href="https://www.wandoujia.com/apps/596157">
            <img src="https://android-artworks.25pp.com/fs08/2026/09/17/7/icon_con_130x130.png" width="68" height="68" alt="微信" class="icon" />
        </a>
    </div>
    <div class="app-desc">
        <h2 class="app-title-h2">
            <a href="https://www.wandoujia.com/apps/596157" class="name">微信</a>
            <span class="tagline wap-hide"></span>
        </h2>
        <div class="meta">
            <span>2.3亿人安装</span>
        </div>
        <div class="comment">
            你可以通过音视频通话、群聊、朋友圈来和朋友们分享生活。
        </div>
    </div>
    <a data-app-id="596157" data-app-vid="605007141" data-app-name="微信" data-app-pname="com.tencent.mm" data-app-vcode="3180" data-app-vname="8.0.78" data-app-icon="https://x/icon.png" class="detail-check-btn">安装</a>
</li>
<li class="search-item search-searchitems">
    <div class="icon-wrap">
        <a href="https://www.wandoujia.com/apps/280155">
            <img src="https://android-artworks.25pp.com/fs08/icon2.png" alt="企业微信" class="icon" />
        </a>
    </div>
    <div class="app-desc">
        <h2 class="app-title-h2"><a href="https://www.wandoujia.com/apps/280155" class="name">企业微信</a></h2>
        <div class="meta"><span>5000万人安装</span></div>
        <div class="comment">办公沟通。</div>
    </div>
    <a data-app-id="280155" data-app-pname="com.tencent.wework" data-app-vcode="990" data-app-vname="4.1.0">安装</a>
</li>
</ul>
)";

static const char* kDetailHtml = R"(
<a class="__normal_realname__ normal-dl-btn __J_default_nr_btn " data-bd-track="detail-common_download_main" data-ch="detail_normal_dl" data-app-name="微信" auto-inspect-button-type="download">普通下载</a>
<a data-app-id="596157" data-app-vid="605007141" data-app-name="微信" data-app-pname="com.tencent.mm" data-app-vcode="3180" data-app-vname="8.0.78" data-app-rtype="0" data-oe="web" data-type="bind">安装</a>
)";

static const char* kHistoryHtml = R"(
<div class="all-version infos" id="allVersion">
<h2 class="block-title">历史版本</h2>
<ul>
<li class="history-list-more">
  <a href="https://www.wandoujia.com/apps/596157/history_v2740"><div class="icon-wrap"><img src="https://x/i.png" alt="微信" /></div><p>微信 8.0.53</p><span>260.81MB</span></a>
  <a data-app-id="596157" data-app-vid="602811104" data-app-name="微信" data-app-pname="com.tencent.mm" data-app-vcode="2740" data-app-vname="8.0.53" class="detail-check-btn" href="https://www.wandoujia.com/apps/596157/history_v2740">查看详情</a>
</li>
<li class="history-list">
  <a href="https://www.wandoujia.com/apps/596157/history_v2500"><p>微信 8.0.40</p><span>250.10MB</span></a>
  <a data-app-id="596157" data-app-pname="com.tencent.mm" data-app-vcode="2500" data-app-vname="8.0.40">查看详情</a>
</li>
</ul>
</div>
)";

static void testWandoujia()
{
    const auto apps = wandoujia::parseSearch(QString::fromUtf8(kSearchHtml));
    CHECK(apps.size() == 2);
    if (apps.size() == 2) {
        const StoreApp& a = apps.at(0);
        CHECK(a.source == QStringLiteral("wandoujia"));
        CHECK(a.package == QStringLiteral("com.tencent.mm"));
        CHECK(a.appId == QStringLiteral("596157"));
        CHECK(a.name == QStringLiteral("微信"));
        CHECK(a.installCount.has_value() && *a.installCount == QStringLiteral("2.3亿人安装"));
        CHECK(a.summary.has_value() && a.summary->contains(QStringLiteral("音视频通话")));
        CHECK(a.iconUrl.has_value() && a.iconUrl->contains(QStringLiteral("android-artworks")));
        CHECK(a.latest.has_value() && a.latest->name == QStringLiteral("8.0.78"));
        CHECK(a.latest->code == 3180);
        CHECK(a.latest->isLatest);
        CHECK(apps.at(1).package == QStringLiteral("com.tencent.wework"));
        CHECK(apps.at(1).latest.has_value() && apps.at(1).latest->code == 990);
    }
    CHECK(wandoujia::parseSearch(
              QStringLiteral("<li class=\"search-item search-searchitems\"><div>广告位</div></li>"))
              .isEmpty());

    const auto latest = wandoujia::latestFromDetail(QString::fromUtf8(kDetailHtml));
    CHECK(latest.has_value());
    if (latest) {
        CHECK(latest->name == QStringLiteral("8.0.78"));
        CHECK(latest->code == 3180);
        CHECK(latest->isLatest);
    }
    CHECK(!wandoujia::latestFromDetail(QStringLiteral("<a>x</a>")).has_value());

    const auto vs = wandoujia::parseHistory(QString::fromUtf8(kHistoryHtml));
    CHECK(vs.size() == 2);
    if (vs.size() == 2) {
        CHECK(vs.at(0).code == 2740);  // 降序
        CHECK(vs.at(0).name == QStringLiteral("8.0.53"));
        CHECK(vs.at(0).size == 273479106);  // 260.81MB
        CHECK(vs.at(1).name == QStringLiteral("8.0.40"));
        CHECK(!vs.at(0).isLatest);
    }

    CHECK(wandoujia::parseSizeCn(QStringLiteral("260.81MB")).value_or(0) / 1024 == 267069);
    CHECK(wandoujia::parseSizeCn(QStringLiteral("1.5GB")).value_or(0) == 1610612736);
    CHECK(wandoujia::parseSizeCn(QStringLiteral("845.6KB")).value_or(0) == 865894);
    CHECK(!wandoujia::parseSizeCn(QStringLiteral("未知")).has_value());

    const QString href = QStringLiteral(
        "https://www.wandoujia.com/apps/596157/binding?downloadUrl=https%3A%2F%2Fandroid-apps."
        "pp.cn%2Ffs08%2Fa.apk%3Fmd5%3D1dae9c615e17a7a0d37e006ab67ee546%26size%3D273481801%"
        "26minSDK%3D23");
    const auto dl = html::queryParam(href, QStringLiteral("downloadUrl"));
    CHECK(dl.has_value() && dl->startsWith(QStringLiteral("https://android-apps.pp.cn/")));
    CHECK(html::queryParam(*dl, QStringLiteral("md5"))
          == QStringLiteral("1dae9c615e17a7a0d37e006ab67ee546"));
    CHECK(html::queryParam(*dl, QStringLiteral("size")) == QStringLiteral("273481801"));
    CHECK(html::queryParam(*dl, QStringLiteral("minSDK")) == QStringLiteral("23"));
}

// ================= F-Droid：镜像 + 索引解析 =================

static void testFdroid()
{
    CHECK(fdroid::mirrorBases(QString()).size() == 3);
    CHECK(fdroid::mirrorBases(QStringLiteral("auto")).size() == 3);
    CHECK(fdroid::mirrorBases(QStringLiteral("tuna")).size() == 1);
    CHECK(fdroid::mirrorBases(QStringLiteral("tuna")).first().second
          == QStringLiteral("https://mirrors.tuna.tsinghua.edu.cn/fdroid/repo/"));
    const auto custom = fdroid::mirrorBases(QStringLiteral("https://example.com/fdroid"));
    CHECK(custom.size() == 1 && custom.first().first == QStringLiteral("custom"));
    CHECK(custom.first().second == QStringLiteral("https://example.com/fdroid/"));
    CHECK(fdroid::mirrorBases(QStringLiteral("nope")).isEmpty());

    const QByteArray sampleJson = R"({
        "apps": [
            {"packageName": "org.fdroid.fdroid", "name": null, "summary": null,
             "icon": "org.fdroid.fdroid.1019061.png", "authorName": "F-Droid", "license": "GPL-3.0",
             "localized": {"en-US": {"name": "F-Droid", "summary": "The app store for FOSS", "description": "LONG TEXT"}}},
            {"packageName": "org.example.plain", "name": "Plain App", "summary": "top-level summary", "icon": ""}
        ],
        "packages": {
            "org.fdroid.fdroid": [
                {"versionName": "1.20", "versionCode": 1019061, "apkName": "org.fdroid.fdroid_1019061.apk",
                 "size": 9000000, "hash": "aa11", "minSdkVersion": 21, "nativecode": ["arm64-v8a"], "added": 1788427366142},
                {"versionName": "1.19", "versionCode": 1018000, "apkName": "org.fdroid.fdroid_1018000.apk",
                 "size": 8000000, "hash": "bb22", "minSdkVersion": 21, "nativecode": ["x86_64"], "added": 1778427366142}
            ],
            "org.example.plain": [
                {"versionName": "2.0", "versionCode": 20, "apkName": "org.example.plain_20.apk",
                 "size": 1000, "hash": "cc33", "minSdkVersion": null, "added": 0}
            ]
        }
    })";
    auto idx = fdroid::parseIndex(sampleJson);
    CHECK(idx.ok());
    if (idx) {
        const fdroid::FdroidIndex& ix = idx.value();
        CHECK(ix.apps.size() == 2);
        const fdroid::AppEntry* fd = nullptr;
        const fdroid::AppEntry* plain = nullptr;
        for (const auto& e : ix.apps) {
            if (e.package == QStringLiteral("org.fdroid.fdroid"))
                fd = &e;
            else if (e.package == QStringLiteral("org.example.plain"))
                plain = &e;
        }
        CHECK(fd && plain);
        if (fd && plain) {
            CHECK(fd->name == QStringLiteral("F-Droid"));  // localized 优先于顶层 null
            CHECK(fd->summary == QStringLiteral("The app store for FOSS"));
            CHECK(fd->license.has_value() && *fd->license == QStringLiteral("GPL-3.0"));
            CHECK(plain->name == QStringLiteral("Plain App"));
            CHECK(plain->summary == QStringLiteral("top-level summary"));
        }
        const QVector<fdroid::VerEntry>& vers = ix.packages.value(QStringLiteral("org.fdroid.fdroid"));
        CHECK(vers.size() == 2);
        if (vers.size() == 2) {
            CHECK(vers.at(0).code == 1019061);  // 新在前
            CHECK(vers.at(0).nativeCode.size() == 1
                  && vers.at(0).nativeCode.at(0) == QStringLiteral("arm64-v8a"));
            CHECK(vers.at(0).addedMs == 1788427366142ULL);
            CHECK(vers.at(0).minSdk.has_value() && *vers.at(0).minSdk == 21);
            CHECK(vers.at(0).hash == QStringLiteral("aa11"));
            CHECK(vers.at(1).code == 1018000);
        }
        const QVector<fdroid::VerEntry>& plainVers =
            ix.packages.value(QStringLiteral("org.example.plain"));
        CHECK(plainVers.size() == 1);
        if (plainVers.size() == 1) {
            CHECK(!plainVers.at(0).minSdk.has_value());
            CHECK(plainVers.at(0).addedMs == 0);
        }
    }
    CHECK(!fdroid::parseIndex(QByteArray("{\"apps\": [")).ok());

    // 缓存 gzip 往返（fdroid-index.json.gz 落盘格式）
    const QByteArray payload = QByteArrayLiteral("hello fdroid index");
    const QByteArray gz = detail::gzipCompress(payload);
    CHECK(!gz.isEmpty());
    auto back = mw::gzipDecompress(gz);
    CHECK(back.ok() && back.value() == payload);
}

// ================= 模型 JSON roundtrip =================

static void testModelJson()
{
    StoreVersion v;
    v.name = QStringLiteral("8.0.78");
    v.code = 3180;
    v.size = 273479106;
    v.minSdk = 23;
    v.addedMs = 1788427366142ULL;
    v.nativeCodes = { QStringLiteral("arm64-v8a"), QStringLiteral("x86_64") };
    v.isLatest = true;
    const QJsonObject j = v.toJson();
    CHECK(j.value(QStringLiteral("minSdk")).toDouble() == 23.0);
    CHECK(j.value(QStringLiteral("addedMs")).toDouble() == 1788427366142.0);
    CHECK(j.value(QStringLiteral("nativeCodes")).toArray().size() == 2);
    CHECK(StoreVersion::fromJson(j).toJson() == j);

    StoreVersion bare;  // optional 全空 → JSON null（serde Option 序列化为 null）
    const QJsonObject jb = bare.toJson();
    CHECK(jb.value(QStringLiteral("minSdk")).isNull());
    CHECK(jb.value(QStringLiteral("addedMs")).isNull());
    CHECK(StoreVersion::fromJson(jb).toJson() == jb);

    StoreApp a;
    a.source = QStringLiteral("wandoujia");
    a.appId = QStringLiteral("596157");
    a.package = QStringLiteral("com.tencent.mm");
    a.name = QStringLiteral("微信");
    a.installCount = QStringLiteral("2.3亿人安装");
    a.latest = v;
    const QJsonObject ja = a.toJson();
    CHECK(ja.value(QStringLiteral("appId")).toString() == QStringLiteral("596157"));
    CHECK(ja.value(QStringLiteral("developer")).isNull());
    CHECK(ja.value(QStringLiteral("latest")).isObject());
    CHECK(StoreApp::fromJson(ja).toJson() == ja);

    StoreApp bareApp;
    bareApp.source = QStringLiteral("fdroid");
    bareApp.package = QStringLiteral("org.fdroid.fdroid");
    const QJsonObject jba = bareApp.toJson();
    CHECK(jba.value(QStringLiteral("latest")).isNull());
    CHECK(StoreApp::fromJson(jba).toJson() == jba);
}

// ================= 内置 zip + AXML =================

static void testApkMin()
{
    const QStringList strings = {
        QStringLiteral("manifest"),    QStringLiteral("package"),
        QStringLiteral("com.example.app"), QStringLiteral("versionName"),
        QStringLiteral("1.2.3"),       QStringLiteral("application"),
        QStringLiteral("label"),       QStringLiteral("示例应用"),
        QStringLiteral("versionCode"),
    };
    TNode application{ "application", { Attr{ "label", TYPE_STRING, 7 } }, {} };
    TNode root{ "manifest",
                { Attr{ "package", TYPE_STRING, 2 },
                  Attr{ "versionCode", TYPE_INT_DEC, 42 },
                  Attr{ "versionName", TYPE_STRING, 4 } },
                { application } };
    const QByteArray axml = buildAxml(strings, root);

    TempApk apk(buildTestZip(axml));
    auto zip = apkmin::ZipArchive::open(apk.path());
    CHECK(zip.ok());
    if (zip) {
        const auto names = zip.value().entryNames();
        CHECK(names.size() == 2);
        CHECK(names.at(0) == QStringLiteral("AndroidManifest.xml")
              && names.at(1) == QStringLiteral("data.bin"));
        auto manifest = zip.value().readEntry(QStringLiteral("AndroidManifest.xml"));
        CHECK(manifest.ok() && manifest.value() == axml);
        auto data = zip.value().readEntry(QStringLiteral("data.bin"));
        CHECK(data.ok() && data.value().size() == 5000 && data.value().at(0) == 'x');
        auto missing = zip.value().readEntry(QStringLiteral("nope.bin"));
        CHECK(missing.ok() && missing.value().isEmpty());

        auto mi = apkmin::parseManifest(axml, apk.path());

        CHECK(mi.ok());
        if (mi) {
            CHECK(mi.value().package == QStringLiteral("com.example.app"));
            CHECK(mi.value().versionCode.has_value() && *mi.value().versionCode == 42);
            CHECK(mi.value().versionName.has_value()
                  && *mi.value().versionName == QStringLiteral("1.2.3"));
            CHECK(mi.value().label.has_value() && *mi.value().label == QStringLiteral("示例应用"));
        }
    }

    // label 为资源引用（@string）→ nullopt 而非报错
    {
        const QStringList refStrings = { QStringLiteral("manifest"), QStringLiteral("package"),
                                         QStringLiteral("com.ref.app"),
                                         QStringLiteral("application"), QStringLiteral("label") };
        TNode app2{ "application", { Attr{ "label", 0x01, 0x7f0e0001 } }, {} };
        TNode root2{ "manifest", { Attr{ "package", TYPE_STRING, 2 } }, { app2 } };
        auto mi = apkmin::parseManifest(buildAxml(refStrings, root2), QStringLiteral("t"));
        CHECK(mi.ok());
        if (mi) {
            CHECK(mi.value().package == QStringLiteral("com.ref.app"));
            CHECK(!mi.value().label.has_value());
        }
    }

    // 嵌套过深的 application 不采信
    {
        const QStringList deepStrings = {
            QStringLiteral("manifest"), QStringLiteral("package"), QStringLiteral("com.deep.app"),
            QStringLiteral("middle"),   QStringLiteral("application"), QStringLiteral("label"),
            QStringLiteral("深层的")
        };
        TNode inner{ "application", { Attr{ "label", TYPE_STRING, 6 } }, {} };
        TNode middle{ "middle", {}, { inner } };
        TNode root3{ "manifest", { Attr{ "package", TYPE_STRING, 2 } }, { middle } };
        auto mi = apkmin::parseManifest(buildAxml(deepStrings, root3), QStringLiteral("t"));
        CHECK(mi.ok());
        if (mi) {
            CHECK(mi.value().package == QStringLiteral("com.deep.app"));
            CHECK(!mi.value().label.has_value());
        }
    }

    CHECK(!apkmin::ZipArchive::open(QStringLiteral("/nonexistent/mwine-store-test.apk")).ok());
    {
        TempApk garbage(QByteArray(1, 1) + QByteArray(1, 2) + QByteArray(1, 3));
        CHECK(!apkmin::ZipArchive::open(garbage.path()).ok());
    }
    CHECK(!apkmin::parseManifest(QByteArrayLiteral("<manifest/>"), QStringLiteral("t")).ok());
    CHECK(!apkmin::parseManifest(QByteArray(), QStringLiteral("t")).ok());
}

// ================= 门面 / UA / 取消 =================

static void testFacade()
{
    CHECK(std::strcmp(UA_BROWSER,
                      "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
                      "(KHTML, like Gecko) Chrome/126.0 Safari/537.36") == 0);
    CHECK(std::strcmp(UA_NEUTRAL, "Houttuynia/1.0") == 0);

    StoreHub hub;
    const auto srcs = hub.sources();
    CHECK(srcs.size() == 2);
    if (srcs.size() == 2) {
        CHECK(srcs.at(0).id == QStringLiteral("fdroid"));
        CHECK(srcs.at(0).name == QStringLiteral("F-Droid"));
        CHECK(srcs.at(0).mirrors.size() == 3);
        if (srcs.at(0).mirrors.size() == 3) {
            CHECK(srcs.at(0).mirrors.at(0).id == QStringLiteral("tuna"));
            CHECK(srcs.at(0).mirrors.at(0).url
                  == QStringLiteral("https://mirrors.tuna.tsinghua.edu.cn/fdroid/repo/"));
            CHECK(srcs.at(0).mirrors.at(1).id == QStringLiteral("nju"));
            CHECK(srcs.at(0).mirrors.at(1).url
                  == QStringLiteral("https://mirror.nju.edu.cn/fdroid/repo/"));
            CHECK(srcs.at(0).mirrors.at(2).id == QStringLiteral("official"));
            CHECK(srcs.at(0).mirrors.at(2).url == QStringLiteral("https://f-droid.org/repo/"));
        }
        CHECK(srcs.at(1).id == QStringLiteral("wandoujia"));
        CHECK(srcs.at(1).name == QStringLiteral("豌豆荚"));
        CHECK(srcs.at(1).mirrors.isEmpty());
    }

    // 未知来源 → 错误文案（search 的未知来源分支不发网络）
    const auto s = hub.search(QStringLiteral("nope"), QStringLiteral("x"), SourceOpts{});
    CHECK(s.first.isEmpty());
    CHECK(s.second.value(QStringLiteral("nope")) == QStringLiteral("未知来源"));
    auto v = hub.versions(QStringLiteral("nope"), QStringLiteral("1"), QStringLiteral("com.x"),
                          SourceOpts{});
    CHECK(v.isErr());
    CHECK(v.error().contains(QStringLiteral("未知来源")));
    auto r = hub.resolve(QStringLiteral("nope"), QStringLiteral("1"), QStringLiteral("com.x"),
                         std::nullopt, QString(), SourceOpts{});
    CHECK(r.isErr());
    CHECK(r.error().contains(QStringLiteral("未知来源")));

    // fetchApk：取消令牌先置位 → 不发网络直接报「已取消」
    Resolved res;
    res.url = QStringLiteral("http://127.0.0.1:1/x.apk");
    res.versionName = QStringLiteral("1.0");
    res.versionCode = 1;
    std::atomic_bool cancel{ true };
    FetchCallbacks cb;
    cb.log = [](const QString&) {};
    const QString nowhere = QDir::temp().filePath(QStringLiteral("mwine-store-test-cancel.apk"));
    auto out = fetchApk(res, nowhere, QStringLiteral("com.example.app"), QStringLiteral("App"),
                        QByteArray(UA_NEUTRAL), SourceOpts{}, cancel, cb);
    CHECK(out.isErr());
    CHECK(out.error() == QStringLiteral("已取消"));
    CHECK(!QFile::exists(nowhere));
}

int main()
{
    testHtml();
    testWandoujia();
    testFdroid();
    testModelJson();
    testApkMin();
    testFacade();
    if (g_failures == 0)
        qInfo("mw_store_test: all passed");
    else
        qWarning("mw_store_test: %d failure(s)", g_failures);
    return g_failures == 0 ? 0 : 1;
}
