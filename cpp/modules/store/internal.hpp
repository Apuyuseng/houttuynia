// 模块内部共享（不对外）：HTTP 管线，对应 crates/mwine-store/src/lib.rs 的
// fetch_text / read_body(_bytes) / probe_download / download_file。
//
// 网络基元走 mw::net（mw::httpRequest / mw::httpDownload）；代理与「跟 302 拿
// 最终直链」两类需求 mw::net 不覆盖，用本地 QNetworkAccessManager 补齐。
#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>

#include <atomic>
#include <functional>

#include "../common/net.hpp"
#include "store.hpp"

namespace mw::store::detail {

// GET（自动跟随重定向；服务端返回 gzip 时透明解码；错误文案对齐 Rust Display）。
// headers 里调用方自带 User-Agent / Accept-Encoding。
mw::Result<QByteArray> fetchBytes(const QString& url,
    const QMap<QByteArray, QByteArray>& headers, int timeoutMs, const QString& proxy);

// GET 文本（浏览器 UA + Accept-Encoding: gzip），对应 lib.rs fetch_text。
mw::Result<QString> fetchText(const QString& url, const SourceOpts& opts, int timeoutMs);

// gzip 压缩（F-Droid 索引落盘缓存用；对应 flate2 GzEncoder）。
QByteArray gzipCompress(const QByteArray& in);

struct ProbeInfo {
    QString finalUrl;   // 跟随重定向后的最终直链
    quint64 total = 0;  // 总字节数（Content-Range/Content-Length，未知 0）
    bool rangeOk = false;  // Range:0-0 是否被满足（206）——续传策略依据
};

/// 探测一个下载链接：GET Range:0-0 跟到最终直链（对应 lib.rs probe_download）。
/// ua 为空时回退浏览器 UA（豌豆荚）；F-Droid 必须传中性 UA（TUNA 反爬浏览器 UA 403）。
mw::Result<ProbeInfo> probeDownload(const QString& url, const SourceOpts& opts,
                                    const QByteArray& ua = {});

/// 下载到文件：Range 断点续传 + 中断重试（服务端不支持 Range 时整档重下）。
/// prog 进度已换算为全量口径并按 1s 节流（对齐 Rust）；sizeHint 为 total 兜底。
/// totalOut（可空）回传探测到的总字节数。
mw::Result<bool> downloadFile(const QString& url, const QString& destPath, const SourceOpts& opts,
    const QByteArray& ua, const std::atomic_bool& cancel,
    const std::function<void(quint64, quint64)>& prog, quint64 sizeHint,
    const std::function<void(const QString&)>& log, quint64* totalOut = nullptr);

}  // namespace mw::store::detail
