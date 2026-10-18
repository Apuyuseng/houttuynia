// Theme —— QML 全局主题单例（QML_SINGLETON，经 Houttuynia 模块注册，
// 在任意 QML 上下文——含 Repeater/ListView delegate——必然可见）。
// 深浅色由 QStyleHints 驱动，changed() 触发全部颜色绑定重算。
#pragma once

#include <QColor>
#include <QJSEngine>
#include <QObject>
#include <QQmlEngine>
#include <QString>

class Theme : public QObject {
    Q_OBJECT
    QML_SINGLETON
    QML_NAMED_ELEMENT(Theme)
    Q_PROPERTY(bool dark READ dark NOTIFY changed)
    Q_PROPERTY(QColor window READ window NOTIFY changed)
    Q_PROPERTY(QColor sidebar READ sidebar NOTIFY changed)
    Q_PROPERTY(QColor card READ card NOTIFY changed)
    Q_PROPERTY(QColor cardHover READ cardHover NOTIFY changed)
    Q_PROPERTY(QColor field READ field NOTIFY changed)
    Q_PROPERTY(QColor text READ text NOTIFY changed)
    Q_PROPERTY(QColor textDim READ textDim NOTIFY changed)
    Q_PROPERTY(QColor textFaint READ textFaint NOTIFY changed)
    Q_PROPERTY(QColor separator READ separator NOTIFY changed)
    Q_PROPERTY(QColor accent READ accent NOTIFY changed)
    Q_PROPERTY(QColor success READ success NOTIFY changed)
    Q_PROPERTY(QColor warning READ warning NOTIFY changed)
    Q_PROPERTY(QColor danger READ danger NOTIFY changed)
    Q_PROPERTY(QColor warnBg READ warnBg NOTIFY changed)
    Q_PROPERTY(QColor warnBorder READ warnBorder NOTIFY changed)
    Q_PROPERTY(QColor termBg READ termBg NOTIFY changed)
    Q_PROPERTY(int radius READ radius CONSTANT)
    Q_PROPERTY(QString mono READ mono CONSTANT)

public:
    static Theme* create(QQmlEngine* qmlEngine, QJSEngine* jsEngine);

    bool dark() const { return m_dark; }
    QColor window() const { return m_dark ? QColor("#1C1C1E") : QColor("#F4F4F6"); }
    QColor sidebar() const { return m_dark ? QColor("#232326") : QColor("#EBEBEE"); }
    QColor card() const { return m_dark ? QColor("#2A2A2E") : QColor("#FFFFFF"); }
    QColor cardHover() const { return m_dark ? QColor("#333338") : QColor("#F4F4F6"); }
    QColor field() const { return m_dark ? QColor("#1A1A1C") : QColor("#FFFFFF"); }
    QColor text() const { return m_dark ? QColor("#ECECEE") : QColor("#1D1D1F"); }
    QColor textDim() const { return m_dark ? QColor("#9A9AA0") : QColor("#8E8E93"); }
    QColor textFaint() const { return m_dark ? QColor("#6B6B70") : QColor("#BDBDBD"); }
    QColor separator() const { return m_dark ? QColor("#3A3A3F") : QColor("#DCDCE0"); }
    QColor accent() const { return m_dark ? QColor("#0A84FF") : QColor("#007AFF"); }
    QColor success() const { return m_dark ? QColor("#30D158") : QColor("#34C759"); }
    QColor warning() const { return m_dark ? QColor("#FFD60A") : QColor("#F0A000"); }
    QColor danger() const { return m_dark ? QColor("#FF453A") : QColor("#FF3B30"); }
    QColor warnBg() const { return m_dark ? QColor("#3B300E") : QColor("#FFF8E1"); }
    QColor warnBorder() const { return m_dark ? QColor("#6B5A1A") : QColor("#FFE082"); }
    QColor termBg() const { return QColor("#1E1E1E"); }
    int radius() const { return 10; }
    QString mono() const
    {
#ifdef Q_OS_MACOS
        return QStringLiteral("Menlo");
#else
        return QStringLiteral("monospace");
#endif
    }

    Q_INVOKABLE QColor avatarColor(const QString& name) const;

signals:
    void changed();

private:
    explicit Theme(QObject* parent = nullptr);
    void watchSystemScheme();

    bool m_dark = false;
};
