// 「出码配对」（Android Studio 同款方向，AOSP docs/dev/adb_wifi.md）：
// Mac 生成 WIFI:T:ADB 二维码；手机（Android 11+ 无线调试）扫码后在
// 手机上以 S 为实例名、P 为密码启动配对服务端（_adb-tls-pairing._tcp）；
// Mac 侧等 mDNS 出现该实例后走既有 `adb pair` 客户端流程配对并连接。
// 配对密码学与子进程封装均在 devices.{hpp,cpp}，此处只做会话生成与等待。
#pragma once

#include <atomic>
#include <functional>

#include <QString>
#include <QVector>

#include "devices.hpp"
#include "result.hpp"

namespace mw::adb {

// 一次出码配对会话（同时是二维码三字段的来源）。
// service/password 字符集限定 [A-Za-z0-9]：规避 ZXing WIFI 载荷转义
//（`;` `:` `\` `,`）、mDNS 实例名空白（adb mdns services 按空白切列）
// 与 CLI 参数转义问题。
struct HostPairSession {
    QString service;   // S：mDNS 实例名，studio-<10 随机字符>（AS 同款前缀）
    QString password;  // P：配对密码，10 随机字符（≈60 bit）
    QString qrPayload; // 二维码文本 "WIFI:T:ADB;S:…;P:…;;"
};

// 生成新会话（系统熵源）。
HostPairSession makeHostPairSession();

// 等待指定实例名的 _adb-tls-pairing 服务出现在发现结果里（轮询注入的
// discover，通常为 discoverMdnsServices）。命中返回 ip:配对端口；
// cancelled 置真返回「已取消」，超时返回错误。
// pollMs 只控制发现频率，取消响应粒度固定 100ms。
mw::Result<QString> waitForPairingInstance(
    const QString& instance,
    const std::function<mw::Result<QVector<MdnsService>>()>& discover,
    int timeoutMs, int pollMs, const std::atomic_bool& cancelled);

} // namespace mw::adb
