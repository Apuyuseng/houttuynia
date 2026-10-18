#include "theme.hpp"

#include <QGuiApplication>
#include <QJSEngine>
#include <QQmlEngine>
#include <QStyleHints>

Theme::Theme(QObject* parent)
    : QObject(parent)
{
}

Theme* Theme::create(QQmlEngine* qmlEngine, QJSEngine* jsEngine)
{
    Q_UNUSED(jsEngine)
    static Theme* instance = nullptr;
    if (!instance) {
        instance = new Theme(qmlEngine);
        instance->watchSystemScheme();
    }
    // 单例由引擎接管生命周期时需设置拥有权；这里经静态持有 + 父子挂引擎
    QQmlEngine::setObjectOwnership(instance, QQmlEngine::CppOwnership);
    return instance;
}

void Theme::watchSystemScheme()
{
    if (auto* app = qGuiApp) {
        if (auto* hints = app->styleHints()) {
            m_dark = hints->colorScheme() == Qt::ColorScheme::Dark;
            connect(hints, &QStyleHints::colorSchemeChanged, this, [this, hints]() {
                const bool dark = hints->colorScheme() == Qt::ColorScheme::Dark;
                if (dark != m_dark) {
                    m_dark = dark;
                    emit changed();
                }
            });
        }
    }
}

QColor Theme::avatarColor(const QString& name) const
{
    static const QColor colors[] = {
        QColor("#5E9CFF"), QColor("#FF8A65"), QColor("#BA68C8"), QColor("#4DB6AC"),
        QColor("#FFB74D"), QColor("#7986CB"), QColor("#F06292"), QColor("#9575CD"),
    };
    int h = 0;
    const QString s = name;
    for (const QChar c : s)
        h = (h * 31 + c.unicode()) % 997;
    return colors[h % 8];
}
