// 真机连接辅助：配对二维码解析、mDNS 服务发现（adb mdns）、
// `adb pair` / `adb tcpip` 子进程封装、捆绑 adb 二进制定位。
//
// 设计约束：配对协议（SPAKE2+ over TLS）实现只存在于 adb 二进制内，
// 因此 pair 走捆绑 platform-tools/adb 子进程；发现走 `adb mdns services`
//（adb server 内建 mDNS 后端，跨平台一致，输出 ip:port 免二次解析）。
#pragma once

#include <QString>
#include <QVector>

#include "result.hpp"

namespace mw::adb {

// `WIFI:T:ADB;S:<服务名>;P:<配对码>;;` 配对二维码（ZXing WIFI 载荷）解析结果。
struct PairQr {
    QString service;  // S：mDNS `_adb-tls-pairing._tcp` 实例名
    QString password; // P：配对码（6 位数字或二维码密钥）
    QString addr;     // 个别厂商扩展的直连地址 host:port（空 = 未提供）
};

// adb mDNS 发现的服务条目。
struct MdnsService {
    QString instance; // 实例名（与配对二维码的 S 字段匹配）
    QString type;     // _adb-tls-pairing._tcp / _adb-tls-connect._tcp / _adb._tcp
    QString host;     // IP
    quint16 port = 0;

    QString addr() const { return QStringLiteral("%1:%2").arg(host).arg(port); }
    bool isPairing() const { return type == QLatin1String("_adb-tls-pairing._tcp"); }
    bool isConnect() const { return type == QLatin1String("_adb-tls-connect._tcp"); }
};

// 解析配对二维码文本。支持：
//  - 标准 `WIFI:T:ADB;S:…;P:…;;`（未知字段忽略，`\;` `\:` `\\` 转义还原）
//  - 裸 `host:port`（仅地址，配对码由调用方另行输入）
mw::Result<PairQr> parsePairQr(const QString& text);

// 解析 `adb mdns services` 输出（每行：实例名 服务类型 ip:port）。
// 容错表头 / 空行 / 非 _adb 行；按 实例名+类型 去重。
QVector<MdnsService> parseMdnsServices(const QString& adbOutput);

// 运行 `adb mdns services`（自动先 `mdns check` 唤醒后端），返回服务列表。
// adb server 不在线会先被 adb 二进制自动拉起。
mw::Result<QVector<MdnsService>> discoverMdnsServices(const QString& adbBin);

// 捆绑 adb 二进制定位：PATH → .app Resources/runtime/platform-tools/adb
//（打包布局）→ 祖先目录下 runtime/platform-tools/adb（开发树）。找不到为空。
QString findAdbBinary();

// `adb pair <addr> <code>` 子进程。成功文案含 "Successfully paired to …"。
mw::Result<QString> runPair(const QString& adbBin, const QString& addr, const QString& code,
                            int timeoutMs = 20000);

// `adb -s <serial> tcpip <port>`（USB 设备转无线监听；成功输出 restarting in TCP mode）。
mw::Result<QString> runTcpip(const QString& adbBin, const QString& serial, quint16 port,
                             int timeoutMs = 15000);

// 形如 host:port 的地址字面量判定（IPv4 / 主机名；IPv6 不支持——adb 无线场景为局域网 IP）。
bool looksLikeAddr(const QString& s);

} // namespace mw::adb
