// mw::Result —— Rust Result<T, E> 的 C++ 对应物。
// 全库统一错误类型 QString（对应 Rust 侧的 string error）。
// 用法：
//   mw::Result<QByteArray> r = mw::readFile(path);
//   if (!r) qWarning() << r.error();
//   use(r.value());
#pragma once

#include <QString>
#include <optional>
#include <type_traits>

namespace mw {

template <typename T>
class Result {
public:
    static Result Ok(T v) { Result r; r.value_ = std::move(v); return r; }
    static Result Err(QString e) { Result r; r.error_ = std::move(e); return r; }

    bool ok() const { return error_.isEmpty(); }
    explicit operator bool() const { return ok(); }
    bool isErr() const { return !ok(); }

    T& value() { return *value_; }
    const T& value() const { return *value_; }
    T valueOr(T def) const { return ok() ? *value_ : std::move(def); }
    const QString& error() const { return error_; }

private:
    Result() = default;
    std::optional<T> value_;
    QString error_;
};

// Result<bool> 特化：纯成功/失败（对应 Rust Result<(), E>）
template <>
class Result<bool> {
public:
    static Result Ok() { return Result(true, {}); }
    static Result Err(QString e) { return Result(false, std::move(e)); }

    bool ok() const { return error_.isEmpty(); }
    explicit operator bool() const { return ok(); }
    bool isErr() const { return !ok(); }
    bool value() const { return true; }
    const QString& error() const { return error_; }

private:
    Result(bool v, QString e) : value_(v), error_(std::move(e)) {}
    bool value_;
    QString error_;
};

// 错误构造捷径：return mw::err<QByteArray>(QStringLiteral("读取失败: %1").arg(path));
template <typename T>
Result<T> err(QString e) { return Result<T>::Err(std::move(e)); }

} // namespace mw
