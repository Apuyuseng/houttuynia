// 日志：统一 [mwine][模块] 前缀，无引号输出（对齐 Rust 侧 println 风格）。
#pragma once

#include <QDateTime>
#include <QDebug>

namespace mw {

inline void logLine(const QString& line)
{
    qInfo().noquote() << QString("[%1] %2")
                             .arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss")),
                                  line);
}

} // namespace mw

#define MW_LOG(module, msg) (::mw::logLine(QStringLiteral("[" module "] ") + (msg)))
