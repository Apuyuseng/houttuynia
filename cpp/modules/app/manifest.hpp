// 二进制 AndroidManifest.xml（AXML）解析：package / versionCode / versionName /
// label（字面量）/ debuggable / minSdk 等 manifest 级信息。
// 对应 crates/mwine-app/src/manifest.rs。只解析 manifest 元素自身的属性——这些值的
// 字符串引用指向 manifest 自己的 string pool（无需 resources.arsc），零 SDK 即可完成。
#pragma once

#include <QByteArray>
#include <QString>

#include <optional>

#include "result.hpp"

namespace mw::app {

// manifest 级元数据（全部为“解析得到什么是什么”，缺失即 nullopt）。
struct ManifestInfo {
    QString package;
    std::optional<QString> versionName;
    std::optional<quint64> versionCode;
    std::optional<QString> label;      // application android:label——仅字面量；@string 引用返回 nullopt（需 arsc）
    std::optional<bool> debuggable;
    std::optional<quint32> minSdk;
    std::optional<quint32> targetSdk;
};

// 解析 AXML 字节（APK 内 AndroidManifest.xml 的解压内容）。label 为错误上下文标签。
mw::Result<ManifestInfo> parseManifest(const QByteArray& axml, const QString& label);

} // namespace mw::app
