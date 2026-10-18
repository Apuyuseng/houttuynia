// mwine-iconfetch —— 图标抓取子进程（守护经 QProcess::execute 调用）。
// 用法：mwine-iconfetch <serial> <package> <outDir>
// 成功产物：<outDir>/<pkg>.png（图标，可能不存在——非 PNG 回退）与
//          <outDir>/<pkg>.label（显示名，可能不存在）
// 退出码：0 = 已尽力（产物按存在与否），2 = 设备/路径级失败。
// 独立进程的意义：APK 解析在守护并发环境下曾有堆损坏（SIGSEGV 循环），
// 隔离后最坏情况只是单个图标缺失，守护本体不受影响。
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryFile>
#include <QThread>

#include "app.hpp"
#include "adb.hpp"

// 轻度盒式模糊×3 ≈ 高斯：抹掉桌面图标细节、保留壁纸色调
static QImage blurImage(QImage img, int radius)
{
    if (img.isNull())
        return img;
    const QImage scaled = img.scaled(540, 960, Qt::KeepAspectRatioByExpanding,
                                     Qt::SmoothTransformation);
    img = scaled.copy(0, 0, qMin(540, scaled.width()), qMin(960, scaled.height()));
    for (int pass = 0; pass < 2; ++pass) {
        const QImage src = img.convertToFormat(QImage::Format_ARGB32);
        QImage dst(src.size(), src.format());
        const int w = src.width(), h = src.height();
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                int r = 0, g = 0, b = 0, n = 0;
                for (int dy = -radius; dy <= radius; dy += radius) {
                    for (int dx = -radius; dx <= radius; dx += radius) {
                        const int sx = qBound(0, x + dx, w - 1);
                        const int sy = qBound(0, y + dy, h - 1);
                        const QRgb c = src.pixel(sx, sy);
                        r += qRed(c); g += qGreen(c); b += qBlue(c); ++n;
                    }
                }
                dst.setPixel(x, y, qRgb(r / n, g / n, b / n));
            }
        }
        img = dst;
    }
    return img;
}

// 壁纸导出（真位图优先）：bridge 接收器在真应用进程读 WallpaperManager，
// 产出无图标、无模糊的壁纸本体；失败回退 HOME 截屏方案。
static bool fetchViaBridge(const mw::adb::AdbClient& adb, const QString& serial,
                           const QString& outPng)
{
    const QString devPath =
        QStringLiteral("/sdcard/Android/data/com.mwine.bridge/files/wallpaper.png");
    // 解冻（新装应用 stopped 态收不到广播）+ 显式组件广播（隐式会被系统拦）
    (void)adb.shellBlocking(
        serial,
        QStringLiteral("am start -n com.mwine.bridge/.UnlockActivity;"
                       " rm -f '%1';"
                       " am broadcast -n com.mwine.bridge/.WallpaperReceiver"
                       " -a com.mwine.bridge.EXPORT_WALLPAPER --es out wallpaper.png"
                       " --ei maxDim 1080").arg(devPath),
        15000);
    QThread::msleep(1500); // 接收器异步落盘
    // 等文件出现（最多 5s）
    for (int i = 0; i < 5; ++i) {
        const auto bytes = adb.pullFile(serial, devPath);
        if (bytes && bytes.value().size() > 1024
            && bytes.value().startsWith(QByteArrayLiteral("\x89PNG"))) {
            const QImage img = QImage::fromData(bytes.value());
            if (!img.isNull()) {
                QFile f(outPng);
                if (f.open(QIODevice::WriteOnly)) {
                    f.write(bytes.value());
                    f.close();
                    return true;
                }
            }
        }
        if (i < 4)
            QThread::msleep(1000);
    }
    (void)adb.shellBlocking(serial, QStringLiteral("rm -f '%1'").arg(devPath), 5000);
    return false;
}

// 壁纸模式：bridge 真图优先 → HOME 截屏回退 → 模糊 → outPng
static int fetchWallpaper(const QString& serial, const QString& outPng)
{
    const mw::adb::AdbClient adb;
    // 真位图：原样保存（不做模糊——本体即壁纸，纱罩负责可读性）
    {
        const QString tmp = outPng + QStringLiteral(".real");
        if (fetchViaBridge(adb, serial, tmp)) {
            QFile::remove(outPng);
            if (QFile::rename(tmp, outPng))
                return 0;
        }
        QFile::remove(tmp);
    }
    (void)adb.shellBlocking(serial,
        QStringLiteral("am start -a android.intent.action.MAIN"
                       " -c android.intent.category.HOME"), 8000);
    QThread::msleep(1200); // 等桌面渲染稳定
    const QString devTmp = QStringLiteral("/sdcard/.mwine_wp.png");
    // 截两次取第二帧：首帧可能拍到壁纸切换动画/过渡
    (void)adb.shellBlocking(serial,
        QStringLiteral("screencap -p %1").arg(devTmp), 15000);
    QThread::msleep(400);
    (void)adb.shellBlocking(serial,
        QStringLiteral("screencap -p %1").arg(devTmp), 15000);
    const auto bytes = adb.pullFile(serial, devTmp);
    (void)adb.shellBlocking(serial, QStringLiteral("rm -f %1").arg(devTmp), 5000);
    if (!bytes || !bytes.value().startsWith(QByteArrayLiteral("\x89PNG")))
        return 3;
    const QImage wall = blurImage(QImage::fromData(bytes.value()), 3);
    if (wall.isNull())
        return 3;
    // 原子落盘：UI 侧 Image 正在读旧文件时不会读到半截
    const QString tmp = outPng + QStringLiteral(".tmp");
    if (!wall.save(tmp, "PNG"))
        return 3;
    QFile::remove(outPng);
    return QFile::rename(tmp, outPng) ? 0 : 3;
}

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    // 壁纸模式：mwine-iconfetch --wallpaper <serial> <out.png>
    if (argc == 4 && qstrcmp(argv[1], "--wallpaper") == 0)
        return fetchWallpaper(argv[2], argv[3]);
    if (argc < 4)
        return 2;
    const QString serial = argv[1], pkg = argv[2], outDir = argv[3];

    const mw::adb::AdbClient adb;
    const auto apkPathR = adb.packageApkPath(serial, pkg);
    if (!apkPathR || apkPathR.value().isEmpty())
        return 2;
    const auto bytesR = adb.pullFile(serial, apkPathR.value());
    if (!bytesR || bytesR.value().isEmpty())
        return 2;

    QTemporaryFile tmp(QDir::temp().filePath(QStringLiteral("mwine-iconfetch-XXXXXX.apk")));
    if (!tmp.open())
        return 2;
    tmp.write(bytesR.value());
    tmp.flush();
    tmp.setAutoRemove(true);

    const auto parsed = mw::app::AdbAppManager::readLocalApk(tmp.fileName());
    if (parsed) {
        // 显示名
        const QString label = parsed.value().manifest.label.value_or(QString());
        if (!label.isEmpty()) {
            QFile lf(QDir(outDir).filePath(pkg + QStringLiteral(".label")));
            if (lf.open(QIODevice::WriteOnly | QIODevice::Truncate))
                lf.write(label.toUtf8());
        }
        // 图标（仅接受 PNG 魔数，webp/xml 等格式 Qt 侧不可靠显示）
        const auto zipR = mw::app::ZipArchive::open(tmp.fileName());
        if (zipR) {
            auto icon = mw::app::AdbAppManager::iconFromZip(zipR.value(),
                                                             parsed.value().badging);
            if (icon && icon->mime == mw::app::IconMime::Png
                && icon->data.startsWith(QByteArrayLiteral("\x89PNG"))) {
                QFile pf(QDir(outDir).filePath(pkg + QStringLiteral(".png")));
                if (pf.open(QIODevice::WriteOnly | QIODevice::Truncate))
                    pf.write(icon->data);
            }
        }
    }
    return 0;
}
