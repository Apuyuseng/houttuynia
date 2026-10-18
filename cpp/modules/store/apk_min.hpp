// 最小 APK 读取：仅读 zip 中央目录提取 AndroidManifest.xml + 解析 AXML 拿
// 包名/版本/label。← crates/mwine-app/src/{apk.rs, manifest.rs} 的最小子集。
//
// 为什么内置（而不 include mw::app）：商店模块与 app 模块解耦（转换期各模块
// 独立编译）；fetchApk 只需要「包名核对 + 显示名兜底」，无需完整 badging。
// TODO(统一)：mw::app 落地后切换为 mw::app 的 ZipArchive/parseManifest。
//
// 覆盖 APK 实际使用的特性：store(0) / deflate(8)、中央目录定位。
// 不支持：ZIP64、加密、跨盘——APK 内目标 entry 不会触发这些。
// 尺寸以中央目录为准（本地头可能带 data descriptor 占位 0）。
#pragma once

#include <QByteArray>
#include <QString>
#include <QVector>

#include <optional>

#include "../common/result.hpp"

namespace mw::store::apkmin {

/// manifest 级元数据（mw::app ManifestInfo 的最小子集；缺失即 nullopt）。
struct ManifestInfo {
    QString package;
    std::optional<QString> versionName;
    std::optional<quint64> versionCode;
    /// application android:label——仅当是字面字符串；@string 引用返回 nullopt（需 arsc）
    std::optional<QString> label;
};

/// 极简 ZIP 读取：open 后按 entry 名提取解压内容。
class ZipArchive {
public:
    /// 读文件并解析中央目录；错误文案对齐 mwine-app（"APK 解析失败（path）: reason"）。
    static mw::Result<ZipArchive> open(const QString& path);

    QVector<QString> entryNames() const;

    /// 提取指定 entry 的解压内容；不存在返回 Ok(空 QByteArray)；数据损坏返回 Err。
    mw::Result<QByteArray> readEntry(const QString& name) const;

private:
    struct Entry {
        QString name;
        quint16 method = 0;
        quint64 compSize = 0;
        quint64 uncompSize = 0;
        quint64 localOffset = 0;
    };
    QByteArray data_;
    QVector<Entry> entries_;
};

/// 解析 AXML 字节（APK 内 AndroidManifest.xml 的解压内容）。label 参数用于错误文案。
mw::Result<ManifestInfo> parseManifest(const QByteArray& axml, const QString& label);

}  // namespace mw::store::apkmin
