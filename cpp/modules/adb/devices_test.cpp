// 真机连接辅助的纯逻辑测试：配对二维码解析、mDNS 服务解析、地址判定。
// （runPair/runTcpip 为子进程封装，真机集成场景由端到端验证，不在此模拟）
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdio>
#include <cstdlib>

#include "devices.hpp"

using namespace mw::adb;

#define CHECK(cond)                                                                       \
    do {                                                                                  \
        if (!(cond)) {                                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
            std::exit(1);                                                                 \
        }                                                                                 \
    } while (0)

static void testParsePairQr()
{
    // 标准形态（AOSP adb_wifi.md：WIFI:T:ADB;S:myname;P:mypass;;）
    {
        const auto r = parsePairQr(QStringLiteral("WIFI:T:ADB;S:adb-27f3c1q-pairing;P:482615;;"));
        CHECK(r.ok());
        CHECK(r.value().service == QStringLiteral("adb-27f3c1q-pairing"));
        CHECK(r.value().password == QStringLiteral("482615"));
        CHECK(r.value().addr.isEmpty());
    }
    // 小写前缀 + 未知字段（M/C）容忍
    {
        const auto r = parsePairQr(
            QStringLiteral("wifi:t:adb;S:phone;P:secret1;M:aa:bb:cc:dd:ee:ff;;"));
        CHECK(r.ok());
        CHECK(r.value().service == QStringLiteral("phone"));
        CHECK(r.value().password == QStringLiteral("secret1"));
    }
    // C 字段携带直连地址
    {
        const auto r = parsePairQr(QStringLiteral("WIFI:T:ADB;S:phone;P:123456;C:192.168.1.42:37831;;"));
        CHECK(r.ok());
        CHECK(r.value().addr == QStringLiteral("192.168.1.42:37831"));
    }
    // 转义还原（服务名含分号）
    {
        const auto r = parsePairQr(QStringLiteral("WIFI:T:ADB;S:a\\;b;P:482615;;"));
        CHECK(r.ok());
        CHECK(r.value().service == QStringLiteral("a;b"));
    }
    // 裸地址形态
    {
        const auto r = parsePairQr(QStringLiteral(" 192.168.1.42:37831 "));
        CHECK(r.ok());
        CHECK(r.value().addr == QStringLiteral("192.168.1.42:37831"));
        CHECK(r.value().service.isEmpty());
    }
    // 错误形态：普通 Wi-Fi 二维码 / 缺配对码 / 空文本
    CHECK(!parsePairQr(QStringLiteral("WIFI:T:WPA;S:home;P:pass;;")).ok());
    CHECK(!parsePairQr(QStringLiteral("WIFI:T:ADB;S:only-service;;")).ok());
    CHECK(!parsePairQr(QString()).ok());
    CHECK(!parsePairQr(QStringLiteral("hello world")).ok());
}

static void testParseMdnsServices()
{
    const QString sample = QStringLiteral(
        "List of discovered mdns services:\n"
        "adb-27f3c1q-pairing _adb-tls-pairing._tcp 192.168.1.42:37831\n"
        "adb-27f3c1q._adb-tls-connect._tcp 192.168.1.42:40113\n"
        "Android-9 _adb._tcp 192.168.1.50:5555\n"
        "\n"
        "garbage line without tokens\n"
        "noise other._tcp 1.2.3.4:9\n"
        "adb-27f3c1q-pairing _adb-tls-pairing._tcp fe80::1:37831\n");
    const auto list = parseMdnsServices(sample);
    CHECK(list.size() == 3);
    CHECK(list.at(0).instance == QStringLiteral("adb-27f3c1q-pairing"));
    CHECK(list.at(0).isPairing());
    CHECK(list.at(0).host == QStringLiteral("192.168.1.42"));
    CHECK(list.at(0).port == 37831);
    CHECK(list.at(0).addr() == QStringLiteral("192.168.1.42:37831"));
    CHECK(list.at(1).isConnect());
    CHECK(list.at(1).port == 40113);
    CHECK(list.at(2).type == QLatin1String("_adb._tcp"));
    CHECK(parseMdnsServices(QStringLiteral("mdns discovery disabled")).isEmpty());
    CHECK(parseMdnsServices(QString()).isEmpty());
}

static void testLooksLikeAddr()
{
    CHECK(looksLikeAddr(QStringLiteral("192.168.1.42:5555")));
    CHECK(looksLikeAddr(QStringLiteral("localhost:5555")));
    CHECK(looksLikeAddr(QStringLiteral("my-host.local:40000")));
    CHECK(!looksLikeAddr(QStringLiteral("192.168.1.42")));
    CHECK(!looksLikeAddr(QStringLiteral("a:b:c")));
    CHECK(!looksLikeAddr(QStringLiteral(":5555")));
    CHECK(!looksLikeAddr(QStringLiteral("host:99999")));
    CHECK(!looksLikeAddr(QString()));
}

int main()
{
    testParsePairQr();
    testParseMdnsServices();
    testLooksLikeAddr();
    printf("devices_test: all passed\n");
    return 0;
}
