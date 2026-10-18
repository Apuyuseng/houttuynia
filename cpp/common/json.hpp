// JSON 构建辅助：QJson 便捷取值/构造（对齐 backend.cpp 用法习惯）。
#pragma once

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace mw {

// 取值（类型安全：缺省返回默认）
inline bool jBool(const QJsonObject& o, const char* k, bool d = false)
{
    const auto v = o.value(QLatin1String(k));
    return v.isBool() ? v.toBool() : d;
}
inline QString jStr(const QJsonObject& o, const char* k)
{
    return o.value(QLatin1String(k)).toString();
}
inline double jNum(const QJsonObject& o, const char* k, double d = 0)
{
    const auto v = o.value(QLatin1String(k));
    return v.isDouble() ? v.toDouble() : d;
}
inline QJsonArray jArr(const QJsonObject& o, const char* k)
{
    return o.value(QLatin1String(k)).toArray();
}
inline QJsonObject jObj(const QJsonObject& o, const char* k)
{
    return o.value(QLatin1String(k)).toObject();
}

inline QJsonObject parseJson(const QByteArray& bytes, bool* ok = nullptr)
{
    const auto doc = QJsonDocument::fromJson(bytes);
    if (ok)
        *ok = !doc.isNull() && doc.isObject();
    return doc.object();
}

inline QByteArray toJsonBytes(const QJsonObject& o, bool compact = true)
{
    return QJsonDocument(o).toJson(compact ? QJsonDocument::Compact : QJsonDocument::Indented);
}

} // namespace mw
