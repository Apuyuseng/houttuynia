// 出码配对纯逻辑测试：会话生成（载荷格式 / 字符集 / 唯一性）、二维码
// 载荷 round-trip（makeHostPairSession → parsePairQr）、mDNS 实例等待
//（注入假发现序列）、二维码矩阵（macOS 上含 CIDetector 解码回读自检）。
#include <QString>
#include <QRegularExpression>
#include <QVector>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>

#include "devices.hpp"
#include "hostpair.hpp"
#include "qrmat.hpp"

using namespace mw::adb;

static const QRegularExpression kServiceRe(QStringLiteral("^studio-[A-Za-z0-9]{10}$"));
static const QRegularExpression kPasswordRe(QStringLiteral("^[A-Za-z0-9]{10}$"));

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            std::exit(1);                                                   \
        }                                                                   \
    } while (0)

static void testMakeSession()
{
    const HostPairSession s = makeHostPairSession();
    // studio- 前缀（AS 同款）+ 10 位随机；密码 10 位；字符集受限
    CHECK(kServiceRe.match(s.service).hasMatch());
    CHECK(kPasswordRe.match(s.password).hasMatch());
    // 载荷与手机端 AdbQrCode 解析器对齐，且 round-trip 还原 S/P
    CHECK(s.qrPayload.startsWith(QStringLiteral("WIFI:T:ADB;S:")));
    CHECK(s.qrPayload.endsWith(QLatin1String(";;")));
    const auto parsed = parsePairQr(s.qrPayload);
    CHECK(parsed.ok());
    CHECK(parsed.value().service == s.service);
    CHECK(parsed.value().password == s.password);
    // 会话间唯一（100 次无重复；service 与 password 均随机）
    std::set<std::string> svcs, pwds;
    for (int i = 0; i < 100; ++i) {
        const HostPairSession x = makeHostPairSession();
        svcs.insert(x.service.toStdString());
        pwds.insert(x.password.toStdString());
    }
    CHECK(svcs.size() == 100);
    CHECK(pwds.size() == 100);
}

static void testWaitForInstance()
{
    // 命中：第 3 次发现出现目标实例（还混有别的不相干服务）
    {
        int calls = 0;
        const auto discover = [&]() -> mw::Result<QVector<MdnsService>> {
            QVector<MdnsService> out;
            if (calls >= 2) {
                MdnsService m;
                m.instance = QStringLiteral("studio-Ab3dE5fG7h");
                m.type = QStringLiteral("_adb-tls-pairing._tcp");
                m.host = QStringLiteral("192.168.1.42");
                m.port = 37831;
                out.append(m);
            } else {
                MdnsService other;
                other.instance = QStringLiteral("adb-XXXX-pairing");
                other.type = QStringLiteral("_adb-tls-pairing._tcp");
                other.host = QStringLiteral("192.168.1.43");
                other.port = 40001;
                out.append(other);
            }
            ++calls;
            return mw::Result<QVector<MdnsService>>::Ok(out);
        };
        std::atomic_bool cancelled{false};
        const auto r = waitForPairingInstance(QStringLiteral("studio-Ab3dE5fG7h"),
                                              discover, 5000, 1, cancelled);
        CHECK(r.ok());
        CHECK(r.value() == QStringLiteral("192.168.1.42:37831"));
        CHECK(calls == 3);
    }
    // 超时：始终无目标实例
    {
        const auto discover = []() {
            return mw::Result<QVector<MdnsService>>::Ok(QVector<MdnsService>{});
        };
        std::atomic_bool cancelled{false};
        const auto r = waitForPairingInstance(QStringLiteral("studio-none"), discover,
                                              40, 1, cancelled);
        CHECK(!r.ok());
        CHECK(r.error().contains(QStringLiteral("超时")));
    }
    // 取消：立即返回「已取消」
    {
        const auto discover = []() {
            return mw::Result<QVector<MdnsService>>::Ok(QVector<MdnsService>{});
        };
        std::atomic_bool cancelled{true};
        const auto r = waitForPairingInstance(QStringLiteral("studio-none"), discover,
                                              5000, 1, cancelled);
        CHECK(!r.ok());
        CHECK(r.error() == QStringLiteral("已取消"));
    }
}

static void testQrMatrix()
{
#ifdef Q_OS_DARWIN
    const QString payload = QStringLiteral("WIFI:T:ADB;S:studio-Ab3dE5fG7h;P:K2mN4pQ6rS;;");
    const QVector<QString> rows = qrMatrixRows(payload);
    CHECK(!rows.isEmpty());
    const int n = rows.size();
    // QR 尺寸 = 21 + 4k（版本 1~40）；方形；每行仅 0/1
    CHECK(n >= 21 && n <= 177);
    CHECK((n - 21) % 4 == 0);
    for (const QString& r : rows) {
        CHECK(r.size() == n);
        for (const QChar c : r)
            CHECK(c == QLatin1Char('0') || c == QLatin1Char('1'));
    }
    // 三个定位图案角为黑
    CHECK(rows.at(0).at(0) == QLatin1Char('1'));
    CHECK(rows.at(0).at(n - 1) == QLatin1Char('1'));
    CHECK(rows.at(n - 1).at(0) == QLatin1Char('1'));
    // 编码→解码回读一致（防方向 / 镜像回归）
    CHECK(qrMatrixSelfTest());
#else
    CHECK(qrMatrixRows(QStringLiteral("x")).isEmpty());
#endif
}

int main()
{
    testMakeSession();
    testWaitForInstance();
    testQrMatrix();
    printf("hostpair_test: all passed\n");
    return 0;
}
