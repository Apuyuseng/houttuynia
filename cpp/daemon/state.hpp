// 守护进程共享状态 + 后台任务模型（对应 daemon.rs 的 AppState 与 api.rs 的任务结构）。
#pragma once

#include <QJsonObject>
#include <QMutex>
#include <QString>
#include <atomic>
#include <memory>
#include <optional>

#include "adb.hpp"
#include "appcache.hpp"
#include "capture.hpp"
#include "devmgr.hpp"
#include "store.hpp"
#include "terminal.hpp"

namespace mw::daemon {

// 任务进度状态（镜像导入/拉取、运行时导入、商店下载共用，对应 api.rs ImageTask / store_api.rs StoreTask）
struct TaskState {
    std::atomic<bool> running{false};
    std::atomic<bool> done{false};
    QString error;             // 读写加锁
    QString result;            // 产物路径（APK/镜像）
    QString package, label, version; // 商店下载元信息
    QString icon;              // 图标文件路径（安装任务：本地 APK 提取；空 = 无）
    std::atomic<quint64> progress{0};
    std::atomic<quint64> progressTotal{0};
    QString phase;
    QStringList lines;         // 日志尾（上限 100 行）

    mutable QMutex mu;       // 保护 error/result/phase/lines（toJson() const 内也需加锁）

    void reset(const QString& title) {
        running.store(true);
        done.store(false);
        progress.store(0);
        progressTotal.store(0);
        QMutexLocker l(&mu);
        error.clear();
        result.clear();
        package.clear();
        label.clear();
        version.clear();
        icon.clear();
        phase = title;
        lines.clear();
        lines.append(title);
    }

    // 原子占位（Rust 侧在锁内 check running → 409 / 置 running）：已占用返回 false。
    bool tryBegin(const QString& title)
    {
        QMutexLocker l(&mu);
        if (running.load())
            return false;
        running.store(true);
        done.store(false);
        progress.store(0);
        progressTotal.store(0);
        error.clear();
        result.clear();
        package.clear();
        label.clear();
        version.clear();
        icon.clear();
        phase = title;
        lines.clear();
        lines.append(title);
        return true;
    }

    // 任务进行中回填展示元信息（安装任务解析出包名/标签/图标后尽早调用，
    // 前端进度卡随即可显示真实应用名与图标；icon 可为空）。
    void setStoreMeta(const QString& pkg, const QString& label_, const QString& ver = {},
                      const QString& iconPath = {})
    {
        QMutexLocker l(&mu);
        package = pkg;
        label = label_;
        version = ver;
        icon = iconPath;
    }

    // 追加日志行并保留尾部 cap 行（Rust：镜像任务 200 / 商店任务 100）。
    void log(const QString& line, int cap = 100) {
        QMutexLocker l(&mu);
        lines.append(line);
        while (lines.size() > cap)
            lines.removeFirst();
    }

    void setProgress(const QString& p, quint64 done_, quint64 total_) {
        QMutexLocker l(&mu);
        phase = p;
        progress.store(done_);
        progressTotal.store(total_);
    }

    void finishOk(const QString& resultPath) {
        QMutexLocker l(&mu);
        result = resultPath;
        phase = QStringLiteral("完成");
        running.store(false);
        done.store(true);
    }

    // 商店下载任务的完成态（Rust StoreTask 额外回填 package/label/version）。
    void finishStoreOk(const QString& resultPath, const QString& pkg, const QString& label_,
                       const QString& ver) {
        QMutexLocker l(&mu);
        result = resultPath;
        package = pkg;
        label = label_;
        version = ver;
        phase = QStringLiteral("完成");
        running.store(false);
        done.store(true);
    }

    void finishErr(const QString& err) {
        QMutexLocker l(&mu);
        error = err;
        running.store(false);
    }

    // 用户确认已读：清错误/完成态（UI"知道了"按钮；否则轮询会把已读错误
    // 重新拉回来，提示永远关不掉）
    void ack() {
        QMutexLocker l(&mu);
        if (running.load())
            return; // 进行中的任务不可 ack
        error.clear();
        result.clear();
        phase.clear();
        done.store(false);
        lines.clear();
    }

    // includeStoreFields：StoreTask 序列化含 package/label/version；
    // ImageTask（镜像/运行时任务）不含（Rust 两个结构体的字段集不同）。
    QJsonObject toJson(bool includeStoreFields = false) const {
        QMutexLocker l(&mu);
        QJsonObject o;
        o[QStringLiteral("running")] = running.load();
        o[QStringLiteral("done")] = done.load();
        o[QStringLiteral("error")] = error;
        o[QStringLiteral("result")] = result;
        if (includeStoreFields) {
            o[QStringLiteral("package")] = package;
            o[QStringLiteral("label")] = label;
            o[QStringLiteral("version")] = version;
            if (!icon.isEmpty())
                o[QStringLiteral("icon")] = icon;
        }
        o[QStringLiteral("phase")] = phase;
        o[QStringLiteral("progress")] = static_cast<double>(progress.load());
        o[QStringLiteral("progress_total")] = static_cast<double>(progressTotal.load());
        QJsonArray arr;
        for (const auto& s : lines)
            arr.append(s);
        o[QStringLiteral("lines")] = arr;
        return o;
    }
};

struct AppState {
    adb::AdbClient adb;

    QMutex vmMu;
    std::optional<quint32> qemuPid;
    QString vmError;      // 空 = 无错误
    QString vmSerial;     // 空 = 未收养

    TaskState imageTask, runtimeTask, storeTask, installTask;
    std::shared_ptr<std::atomic_bool> imageCancel = std::make_shared<std::atomic_bool>(false);
    std::shared_ptr<std::atomic_bool> storeCancel = std::make_shared<std::atomic_bool>(false);
    std::shared_ptr<std::atomic_bool> runtimeCancel = std::make_shared<std::atomic_bool>(false);

    capture::Session capture;

    // 应用注册表 + 图标缓存（按当前 AVD 键控；VM 重置时 wipe）
    AppCache* appCache();
    AppCache* m_appCache = nullptr;
    QString m_appCacheKey;
    term::TermManager terminals;
    store::StoreHub storeHub;
    dev::DeviceManager devices; // 真机连接（USB / 扫码配对 / 配对码 / 手动）

    // 便捷访问（加锁）
    QString currentSerial();          // 收养的 serial 或空
    bool vmRunning();                 // adb 在线 emulator 判定（与 Rust status() 同语义）
    void setVmError(const QString&);
    QString vmErrorLocked();
    void adoptSerial(const QString&);
};

} // namespace mw::daemon
