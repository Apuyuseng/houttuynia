// 真机连接管理：设备枚举（USB + 无线）、getprop 富化、配对（扫码 / 配对码 /
// 手动地址）、USB 转无线、已知设备档案持久化（~/.mwine/devices.json）。
//
// 真机一律经 adb server 通道（无线 TLS 依赖配对密钥，仅 server 掌握；
// USB 由 server 的 libusb 后端接管），与模拟器的 direct 直连路径并存互不影响。
#pragma once

#include <atomic>
#include <thread>

#include <QHash>
#include <QJsonObject>
#include <QJsonArray>
#include <QMutex>
#include <QString>

#include "hostpair.hpp"
#include "result.hpp"

namespace mw::dev {

// 已知设备档案（配对过 / 连接过的真机；按稳定 key（ro.serialno 优先）持久化）
struct KnownDevice {
    QString key;      // ro.serialno（无则退回 serial）
    QString model;
    QString brand;
    QString android;
    QString lastAddr; // 最近无线连接地址 ip:port
    qint64 lastSeen = 0;
};

class DeviceManager {
public:
    // 枚举在线真机：adb devices（过滤 emulator）+ getprop 富化（5 分钟缓存）
    // + 已知档案合并。adb server 不在线时自动拉起（捆绑二进制）。
    mw::Result<QJsonArray> listDevices();

    // mDNS 发现的 adb 服务（pairing / connect 两类）
    mw::Result<QJsonArray> discovered();

    // 配对并自动连接。请求三选一：
    //   {qr: "WIFI:T:ADB;S:…;P:…;;"}          扫码结果直配
    //   {code: "482615", service: "实例名"}    配对码 + mDNS 服务（省略且唯一时自动选）
    //   {code: "482615", addr: "ip:配对端口"} 配对码 + 手输地址
    mw::Result<QJsonObject> pair(const QJsonObject& req);

    // 连接已配对设备（adb connect ip:port），成功返回设备 JSON
    mw::Result<QJsonObject> connectAddr(const QString& addr);

    // 断开无线连接（adb disconnect）；USB 设备无此操作
    mw::Result<bool> disconnect(const QString& serial);

    // 忘记设备：删除档案；在线无线连接一并断开
    mw::Result<bool> forget(const QString& serialOrAddr);

    // USB 一键转无线：adb tcpip <port> → 取设备 wlan IP → connect
    mw::Result<QJsonObject> usbToWifi(const QString& serial, quint16 port = 5555);

    // ===== 出码配对（Android Studio 同款方向）=====
    // 生成 WIFI:T:ADB 二维码会话并后台等待手机扫码（mDNS 出现 studio-xxx
    // 配对服务）→ adb pair → 自动连接。同一时刻仅一个会话：进行中重复
    // begin 返回当前会话（不换码）；终态后再 begin 开新会话。
    mw::Result<QJsonObject> hostPairBegin(int waitMs = 180000);
    QJsonObject hostPairStatus(); // {state, service, qr, rows, message, ...}
    bool hostPairCancel();        // 取消等待（配对/连接中则顺其自然完成）

    ~DeviceManager(); // 收掉出码配对后台线程
private:
    QString storePath() const;
    void load();
    void saveLocked();
    void upsertKnown(const QString& serial, const QJsonObject& info);

    // getprop 富化（unauthorized / offline 返回空字段）
    QJsonObject deviceInfo(const QString& serial);
    // 等待 serial 出现在 adb devices 且状态 device（轮询）
    bool waitOnline(const QString& serial, int timeoutMs);
    // 配对成功后的连接：轮询 mDNS 新增 connect 服务 + adb devices 双通道
    mw::Result<QString> connectAfterPair(const QVector<QString>& beforeConnectAddrs);
    // 配对结果 JSON（pair() 与出码配对 worker 共用）
    QJsonObject pairOutcome(bool connected, const QString& connectAddr,
                            const QString& pairAddr, const QString& pairService,
                            const QString& message);
    void hostPairWorker(adb::HostPairSession session, QJsonArray rows,
                        QVector<QString> beforeConnectAddrs, int waitMs);

    mutable QMutex m_mu;
    QHash<QString, KnownDevice> m_known;          // key → 档案
    QHash<QString, QPair<qint64, QJsonObject>> m_infoCache; // serial → (时间, getprop)
    bool m_loaded = false;

    // 出码配对会话状态（status 快照 + 后台线程；state ∈ waiting/pairing/
    // connecting/done/failed/cancelled，初始 {} 表示无会话）
    QMutex m_hostMu;
    QJsonObject m_hostStatus;
    std::atomic_bool m_hostActive{false};
    std::atomic_bool m_hostCancel{false};
    std::thread m_hostThread;
};

} // namespace mw::dev
