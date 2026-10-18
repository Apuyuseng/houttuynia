// mwine CLI 轻量参数解析（clap derive 的对齐子集）。
// ← crates/mwine-cli/src/main.rs 的 #[derive(Parser)] 结构
//
// 支持形态：
//   位置参数          —— 按出现顺序 take()/rest() 消费
//   --flag            —— 布尔开关 has()
//   --opt value       —— 单值选项 opt()（也接受 --opt=value）
//   --app a --app b   —— 可重复选项 multi()
//   "--" 之后          —— 全部按位置参数（vm start 的 last=true extra）
//   选项名别名        —— {"mem-mb","mem_mb"} 等价匹配（clap alias）
//
// 与 Rust 版的差异：不做类型推断/自动报错，各命令处理函数自行校验
//（未知 flag / 缺参数 → exit 2 + 用法提示，行为对齐 clap）。
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

namespace mwine {

class Args {
public:
    Args() = default;
    explicit Args(const QStringList& argv)
    {
        bool positionalOnly = false;
        for (const QString& raw : argv) {
            Tok t;
            t.raw = raw;
            if (!positionalOnly && raw == QLatin1String("--")) {
                positionalOnly = true;
                t.consumed = true; // 分隔符本身不参与
                toks_.append(t);
                continue;
            }
            if (!positionalOnly && raw.startsWith(QLatin1String("--"))) {
                t.isFlag = true;
                const QString body = raw.mid(2);
                const int eq = body.indexOf(QLatin1Char('='));
                if (eq >= 0) {
                    t.name = body.left(eq);
                    t.inlineValue = body.mid(eq + 1);
                    t.hasInline = true;
                } else {
                    t.name = body;
                }
            }
            toks_.append(t);
        }
    }

    // 下一个未消费的位置参数（无则 nullopt）
    std::optional<QString> take()
    {
        for (auto& t : toks_) {
            if (t.consumed || t.isFlag)
                continue;
            t.consumed = true;
            return t.raw;
        }
        return std::nullopt;
    }

    // 剩余全部未消费的位置参数（extra / 错误提示用）
    QStringList rest() const
    {
        QStringList out;
        for (const auto& t : toks_) {
            if (!t.consumed && !t.isFlag)
                out.append(t.raw);
        }
        return out;
    }

    // 布尔 flag：names 任一命中即真（clap alias）
    bool has(const QStringList& names) const
    {
        for (const auto& t : toks_) {
            if (t.isFlag && !t.consumed && names.contains(t.name))
                return true;
        }
        return false;
    }
    bool has(const char* name) const { return has(QStringList{QString::fromLatin1(name)}); }

    // 单值选项：取值（inline 或后随 token），重复出现取首个（clap 单值语义）
    std::optional<QString> opt(const QStringList& names)
    {
        for (int i = 0; i < toks_.size(); ++i) {
            auto& t = toks_[i];
            if (!t.isFlag || t.consumed || !names.contains(t.name))
                continue;
            t.consumed = true;
            if (t.hasInline)
                return t.inlineValue;
            if (i + 1 < toks_.size()) {
                auto& v = toks_[i + 1];
                if (!v.consumed) {
                    v.consumed = true;
                    return v.raw;
                }
            }
            return QString(); // 缺值：返回空串（调用方按需校验）
        }
        return std::nullopt;
    }
    std::optional<QString> opt(const char* name)
    {
        return opt(QStringList{QString::fromLatin1(name)});
    }

    // 可重复选项（capture start --app）
    QStringList multi(const QStringList& names)
    {
        QStringList out;
        for (int i = 0; i < toks_.size(); ++i) {
            auto& t = toks_[i];
            if (!t.isFlag || t.consumed || !names.contains(t.name))
                continue;
            t.consumed = true;
            if (t.hasInline) {
                out.append(t.inlineValue);
                continue;
            }
            if (i + 1 < toks_.size() && !toks_[i + 1].consumed) {
                toks_[i + 1].consumed = true;
                out.append(toks_[i + 1].raw);
            }
        }
        return out;
    }

    // 解析后仍残留的未识别 --flag（用法错误检测）
    bool unknownFlag(QString* name = nullptr) const
    {
        for (const auto& t : toks_) {
            if (t.isFlag && !t.consumed) {
                if (name)
                    *name = t.name;
                return true;
            }
        }
        return false;
    }

    QStringList raw() const
    {
        QStringList out;
        for (const auto& t : toks_)
            out.append(t.raw);
        return out;
    }

private:
    struct Tok {
        QString raw;
        bool isFlag = false;
        QString name;
        QString inlineValue;
        bool hasInline = false;
        bool consumed = false;
    };
    QVector<Tok> toks_;
};

} // namespace mwine
