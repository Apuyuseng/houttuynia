#include <QGuiApplication>
#include <QImage>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSettings>
#include <QDateTime>
#include <QDir>
#include <QTimer>
#include <QTranslator>
#include <QSocketNotifier>

#include <atomic>
#include <csignal>
#include <unistd.h>

#include "backend.h"

namespace {
// SIGTERM/SIGINT → 优雅退出（aboutToQuit 链：存档关停 VM）。signal handler
// 里只能写 pipe；GUI 线程经 QSocketNotifier 唤醒后 quit()
int g_sigPipe[2] = { -1, -1 };
void sigToQuit(int)
{
    const char q = 'q';
    (void)::write(g_sigPipe[1], &q, 1);
}
} // namespace

int main(int argc, char *argv[])
{
    QGuiApplication app(argc, argv);
    app.setApplicationName("Houttuynia");
    app.setOrganizationName("Houttuynia");
    app.setApplicationVersion("0.3.0-qt");

    // Fusion 全平台：界面为自绘色板（Theme 单例），原生 macOS style 禁止自定义
    // 控件背景且其 BusyIndicator 依赖 webp 插件（brew Qt 未带）
    QQuickStyle::setStyle("Fusion");

    Backend backend;
    // 启动语言：QSettings 持久化值；UI 回归钩子 MWINE_LANG 优先（如 en）
    {
        QString lang = qEnvironmentVariable("MWINE_LANG");
        if (lang.isEmpty())
            lang = QSettings().value(QStringLiteral("ui/language")).toString();
        if (!lang.isEmpty() && lang != QStringLiteral("zh")) {
            auto* t = new QTranslator(&app);
            if (t->load(QStringLiteral(":/i18n/") + lang + QStringLiteral(".qm")))
                qApp->installTranslator(t);
            else
                delete t;
        }
    }

    QQmlApplicationEngine engine;
    backend.attachEngine(&engine);
    engine.rootContext()->setContextProperty("backend", &backend);
    engine.rootContext()->setContextProperty("qtVersionStr", QString::fromLatin1(qVersion()));
    // 关于页版本胶囊（与 setApplicationVersion 同源）
    engine.rootContext()->setContextProperty("appVersionStr", app.applicationVersion());

    // 自验证钩子一：MWINE_SCREENSHOT=<路径> 时切到 MWINE_PAGE 页，延时截取后退出。
    // initialPage 必须在 load 之前注入：Main 的 Component.onCompleted 在加载期同步执行
    {
        const int page = qEnvironmentVariableIntValue("MWINE_PAGE", nullptr);
        if (page >= 0)
            engine.rootContext()->setContextProperty(QStringLiteral("initialPage"), page);
        // 自验证钩子：MWINE_OPEN_STORE=1 时启动即弹「在线下载」独立窗口（应用库入口回归用）
        if (qEnvironmentVariableIsSet("MWINE_OPEN_STORE"))
            engine.rootContext()->setContextProperty(QStringLiteral("openStoreOnStart"), true);
        // 自验证钩子：MWINE_DEMO_TASK=1 时镜像页用假任务数据演示进度卡
        //（下载中态 UI 回归用，不必真拉 1GB 镜像）
        if (qEnvironmentVariableIsSet("MWINE_DEMO_TASK"))
            engine.rootContext()->setContextProperty(QStringLiteral("demoTaskOn"), true);
        // 自验证钩子：MWINE_OPEN_DRAWER=1 时镜像页启动即开「在线镜像」抽屉
        if (qEnvironmentVariableIsSet("MWINE_OPEN_DRAWER"))
            engine.rootContext()->setContextProperty(QStringLiteral("openDrawerOnStart"), true);
        // 自验证钩子：MWINE_OPEN_ABOUT=1 时启动即切到「关于 / 许可」页（LGPL 入口回归用）
        if (qEnvironmentVariableIsSet("MWINE_OPEN_ABOUT"))
            engine.rootContext()->setContextProperty(QStringLiteral("openAboutOnStart"), true);
        // 自验证钩子：MWINE_QR_DIALOG=1 时启动即打开真机「出码配对」二维码弹窗
        if (qEnvironmentVariableIsSet("MWINE_QR_DIALOG"))
            engine.rootContext()->setContextProperty(QStringLiteral("openQrDialogOnStart"), true);
    }

    engine.loadFromModule("Houttuynia", "Main");

    if (engine.rootObjects().isEmpty())
        return -1;

    {
        const QString shotPath = qEnvironmentVariable("MWINE_SCREENSHOT");
        if (!shotPath.isEmpty()) {
            auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
            if (!window)
                return 2;
            QTimer::singleShot(6000, window, [window, shotPath]() {
                // 与 MWINE_OPEN_STORE 组合时优先截「在线下载」子窗口（Loader 弹出）
                QQuickWindow *target = window->findChild<QQuickWindow *>();
                (target ? target : window)->grabWindow().save(shotPath);
                qApp->quit();
            });
        }
    }

    // 自验证钩子二：MWINE_SHOTDIR=<目录> 时常驻，收到 SIGUSR1 截图到该目录（点击回归用）
    const QString shotDir = qEnvironmentVariable("MWINE_SHOTDIR");
    if (!shotDir.isEmpty()) {
        static std::atomic_bool shotReq{false};
        std::signal(SIGUSR1, [](int) { shotReq.store(true); });
        auto *window2 = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
        auto *poller = new QTimer(&app);
        QObject::connect(poller, &QTimer::timeout, &app, [window2, shotDir]() {
            if (!shotReq.exchange(false))
                return;
            const QString path = QDir(shotDir).filePath(
                QStringLiteral("shot-") + QDateTime::currentDateTime().toString(QStringLiteral("HHmmss-zzz")) + QStringLiteral(".png"));
            window2->grabWindow().save(path);
        });
        poller->start(300);
    }

    // 自验证钩子三：MWINE_INSTALL_APK=<路径> 时经 UI 后端发起安装（顶栏进度态回归用）
    {
        const QString apk = qEnvironmentVariable("MWINE_INSTALL_APK");
        if (!apk.isEmpty())
            QTimer::singleShot(2500, &backend,
                               [&backend, apk]() { backend.installApk(apk, false); });
    }

    // 退出时收尾：先存档关停 VM（自拉 daemon 场景，见 shutdownForQuit），
    // 再停按键映射 / 终端会话（守护进程随 QProcess 析构终止）
    QObject::connect(&app, &QCoreApplication::aboutToQuit, &backend, [&backend]() {
        backend.shutdownForQuit();
        backend.keywatchStop();
        backend.closeTerminal();
    });

    // SIGTERM/SIGINT 优雅化：kill / Ctrl-C 也走 aboutToQuit 链
    if (::pipe(g_sigPipe) == 0) {
        auto* notifier = new QSocketNotifier(g_sigPipe[0], QSocketNotifier::Read, &app);
        QObject::connect(notifier, &QSocketNotifier::activated, &app,
                         [&app]() { QCoreApplication::quit(); });
        std::signal(SIGTERM, sigToQuit);
        std::signal(SIGINT, sigToQuit);
    }

    return app.exec();
}
