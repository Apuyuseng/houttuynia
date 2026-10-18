// aapt2 `dump badging` 输出解析与 aapt2 定位（可选增强路径）。
// 对应 crates/mwine-app/src/badging.rs：产品形态无 SDK 时 aapt2 不存在——
// 模块主路径不依赖它；开发机上有 build-tools 时用它富化标签/图标。
#pragma once

#include <QString>
#include <QStringList>

#include <optional>

#include "result.hpp"

namespace mw::app {

// badging 解析结果。
struct ApkBadging {
    QString package;
    std::optional<QString> label;
    std::optional<QString> icon; // 最高密度图标的 APK 内 entry 路径（可能是自适应图标的 .xml）
};

// 位图图标扩展名判断（png/webp/jpg/jpeg）。
bool isRasterIcon(const QString& path);

// 解析 `aapt2 dump badging` 全文输出。
ApkBadging parseBadging(const QString& text);

// adaptive icon（.xml）在 APK 里通常伴随同名位图；扫描 zip 目录找最高密度位图。
std::optional<QString> pickRasterFallback(const QStringList& entries, const QString& xmlIcon);

// 定位 aapt2：MWINE_AAPT2 环境变量 > PATH > ANDROID_SDK_ROOT/ANDROID_HOME
// > macOS 默认 SDK 路径（~/Library/Android/sdk）。版本号目录取字典序最大。
// 找不到返回 nullopt（调用方走无 SDK 路径）。
std::optional<QString> findAapt2();

// 用 aapt2 提取 badging（工具不存在/执行失败返回 nullopt，不视为错误）。
std::optional<ApkBadging> aapt2Badging(const QString& aapt2, const QString& apkPath);

} // namespace mw::app
