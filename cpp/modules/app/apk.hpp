// 极简 ZIP 读取：只做 APK 内单 entry 提取（AndroidManifest.xml / 图标）。
// 对应 crates/mwine-app/src/apk.rs。覆盖 store(0)/deflate(8) + 中央目录定位；
// 不支持 ZIP64/加密/跨盘。尺寸以中央目录为准（本地头可能带 data descriptor 占位 0）。
#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <optional>
#include <vector>

#include "result.hpp"

namespace mw::app {

// raw deflate（zip method 8，无 zlib 头）解压。
// 说明：zlib 的 inflateInit2 正窗口（15）期望 zlib 流；zip 内是 raw 流，
// 窗口位必须取负值（-15）。mw::gzipDecompress 只覆盖 gzip 流，故模块内自备本辅助。
mw::Result<QByteArray> rawDeflate(const QByteArray& compressed);

// raw deflate 压缩（构造 zip 测试数据用；业务只解不压）。
mw::Result<QByteArray> rawCompress(const QByteArray& data);

class ZipArchive {
public:
    // 整文件读入后解析中央目录。
    static mw::Result<ZipArchive> open(const QString& path);
    static mw::Result<ZipArchive> fromBytes(const QByteArray& data, const QString& label);

    ZipArchive() = default;
    ZipArchive(ZipArchive&&) = default;
    ZipArchive& operator=(ZipArchive&&) = default;
    ZipArchive(const ZipArchive&) = delete;
    ZipArchive& operator=(const ZipArchive&) = delete;

    QStringList entryNames() const;
    bool hasEntry(const QString& name) const;

    // 提取指定 entry 的解压内容；不存在返回 nullopt；数据损坏返回 Err。
    mw::Result<std::optional<QByteArray>> readEntry(const QString& name) const;

private:
    struct CentralEntry {
        QString name;
        quint16 method = 0;
        quint64 compSize = 0;
        quint64 uncompSize = 0;
        quint64 localOffset = 0;
    };

    static bool readCentralDirectory(const QByteArray& data, std::vector<CentralEntry>& out,
                                     QString& err);

    QByteArray data_;
    std::vector<CentralEntry> entries_;
};

} // namespace mw::app
