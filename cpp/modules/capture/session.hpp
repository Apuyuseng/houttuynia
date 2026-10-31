// 采集会话编排 ← crates/mwine-capture/src/session.rs
//
// 两种采集形态（对应同一 Session 实例，互斥占用）：
// - 本机模式 start()：ecapture 作为本机常驻子进程拉起（QProcess），
//   stdout 行流直接喂解析器；
// - 设备模式 startOnDevice()：mwine-adb 自研协议栈直连 guest——
//   1. ensureReady()：guest root 化 + 推送 ecapture 二进制；
//   2. nohup 拉起 `ecapture tls --libssl=<conscrypt APEX> --eventaddr=<file>`；
//   3. 采集线程常驻 `tail -c +N -f` shell 会话 → 增量解析 → flow 重组 → SQLite；
//      看门狗线程低频刷新 pid→包名富化映射（pm list packages -U + ps）、
//      空闲 flow 定稿、WebView 旁观产物可观测性；
//   4. WebView 旁观（纯被动）：Chromium 命令行开关 + 密钥日志 + tcpdump pcap
//      （stop() 时停 pcap，pcap_decrypt 在宿主侧解密入库/导出）；
//   5. stop()：杀 guest 进程 + WebView 密文侧终局解密入库 + 冲刷 live flow。
//
// 目标过滤在查询侧完成（package/comm 字段），不在采集侧丢弃——保留全量证据。
#pragma once

#include <QJsonObject>
#include <QMap>
#include <QMutex>
#include <QScopedPointer>
#include <QString>
#include <QVector>
#include <atomic>
#include <thread>

#include "result.hpp"
#include "store.hpp"

namespace mw::capture {

// guest 侧路径常量 ← Rust session.rs 顶部。
inline constexpr const char* kGuestBin = "/data/local/tmp/ecapture";
inline constexpr const char* kGuestLog = "/data/local/tmp/mwine-ecap.log";
inline constexpr const char* kGuestErr = "/data/local/tmp/mwine-ecap.err";
// Android 13 系统 conscrypt（Java 层 TLS 全走这里）
inline constexpr const char* kConscryptLibssl = "/apex/com.android.conscrypt/lib64/libssl.so";
// WebView 旁观（纯被动）：Chromium 命令行开关 + 密钥日志 + 抓包。
// 依据：Chromium issue 41203702（Android 用 flag 替代 SSLKEYLOGFILE 环境变量，
// WebView 经 /data/local/tmp/webview-command-line 读取，仅 debuggable 构建生效——
// userdebug 镜像满足）。pcap + 密钥可在宿主侧解密。
inline constexpr const char* kGuestWvCmdline = "/data/local/tmp/webview-command-line";
inline constexpr const char* kGuestWvKeys = "/data/local/tmp/webview-keys.log";
inline constexpr const char* kGuestWvPcap = "/data/local/tmp/mwine-webview.pcap";

// WebView 密文侧收尾纯逻辑：pcap + 密钥日志 → 宿主 TLS 解密 → 合成 pid 的
// webview 事件 → Reassembler 重组出的 flow 列表（comm 标为 "WebView(解密)"）。
// 空产物（无 WebView 流量/密钥未生成）返回空。设备 stop 与测试共用。
QVector<Flow> webviewFlowsFromPcap(const QByteArray& pcap, const QByteArray& keylogText);

// 采集目标：packages 为空表示全量（过滤交给查询侧）。
struct TargetSpec {
    QVector<QString> packages;
};

class Session {
public:
    Session();
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    // 启动采集（本机子进程模式；已在运行 → Err("会话已在运行")）。
    mw::Result<bool> start(const QString& serial, const TargetSpec& targets);
    // 启动采集（设备模式，mwine-adb 直连 guest；已在运行同样报错）。
    // 失败原因落 lastError（未 root/无二进制/启动失败）。
    mw::Result<bool> startOnDevice(const QString& serial, const TargetSpec& targets);
    // 停止采集（未在运行 → Err("会话未在运行")）。设备模式附带 WebView 密文侧
    // 终局：拉 pcap + 密钥 → 宿主解密 → 明文 flows 入库。
    mw::Result<bool> stop();

    // 状态快照（字段与 Rust CaptureStatus serde 序列化一致）。
    QJsonObject status() const;

    // 查询已落库 flow（每项 = FlowSummary toJson）。
    mw::Result<QVector<QJsonObject>> queryFlows(const FlowsQuery& q) const;
    // 筛选条件下的总条数（分页用）。
    mw::Result<quint64> totalFlows(const FlowsQuery& q) const;
    // 详情：exchanges 全量（含明文 body）。未命中返回空对象。
    mw::Result<QJsonObject> flowDetail(qint64 id) const;
    // live flow 快照（运行中 UI 的实时页；每项 = FlowSummary toJson）。
    QVector<QJsonObject> liveFlows() const;
    // 解析质量统计（kind 分布/解析率）。
    QJsonObject stats() const;
    // 清空落库数据。
    mw::Result<bool> clear() const;

    // —— 设备模式外围 ——
    // 部署前置：guest root 化 + 推送 ecapture 二进制（幂等）。
    mw::Result<bool> ensureReady(const QString& serial);
    // 导出 WebView 旁观包（密钥 + pcap + Wireshark 使用说明）到宿主目录
    // ~/.mwine/capture/webview-<ts>/，返回目录路径。serial 未设置 → Err。
    // 密钥/pcap 可能尚未生成（还没有 WebView 流量）——空文件占位。
    mw::Result<QString> exportWebviewBundle() const;

    // 包名 → uid（`pm list packages -U`）。
    static QHash<QString, quint32> packageUids(const QString& serial);
    // pid → 包名（uid 表 + ps 联合）。
    static QMap<quint32, QString> pidPackages(const QString& serial);
    // 宿主侧 ecapture 查找（bundle Resources/bin → 可执行同目录 → ~/.mwine/bin；
    // daemon 组件状态接口用：空 = 未安装，前端引导下载）
    static QString findHostBinary();

    // 数据库路径注入（默认 ~/.mwine/capture/flows.db；须在未运行时设置）。
    void setDbPath(const QString& path);

private:
    struct Shared;

    static void collectorLoop(Shared* sh, const QString& bin);
    static void watchdogLoop(Shared* sh);
    static void processEvents(Shared* sh, const QVector<Event>& events);
    // 设备模式线程体
    static void deviceCollectorLoop(Shared* sh);
    static void deviceWatchdogLoop(Shared* sh);
    // WebView 密文侧终局：拉 pcap + 密钥 → webviewFlowsFromPcap → 入库。
    static void ingestWebviewFlows(const QString& serial, Shared* sh);
    // 设备模式 guest 侧准备工作（root/清理/拉起/旁观），成功后建线程。
    mw::Result<bool> startOnDeviceUnlocked(const QString& serial);

    mutable QMutex mutex_;
    bool running_ = false;
    bool deviceMode_ = false; // stop()/看门狗按此分支
    QString serial_;
    QString lastError_;
    QString dbPath_;
    QVector<QString> targets_;
    QScopedPointer<Shared> shared_;
    std::thread collector_;
    std::thread watchdog_;
};

} // namespace mw::capture
