// 文件系统工具：~/.mwine 布局 + 常用读写（原子写）。
#pragma once

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTemporaryFile>

#include "result.hpp"

namespace mw {

inline QString homeDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
}

inline QString mwineDir()
{
    return QDir(homeDir()).filePath(QStringLiteral(".mwine"));
}

inline mw::Result<QByteArray> readFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return mw::err<QByteArray>(QStringLiteral("无法读取 %1: %2").arg(path, f.errorString()));
    return mw::Result<QByteArray>::Ok(f.readAll());
}

inline mw::Result<bool> writeFile(const QString& path, const QByteArray& data)
{
    // 原子写：临时文件 + rename，避免半写状态。
    // 注意：rename 落位后必须关掉 autoRemove，否则 QTemporaryFile 析构会把
    // 目标文件当自己的临时文件删掉（Qt 6.11.1 实测数据丢失）。
    QTemporaryFile tmp(QFileInfo(path).absolutePath() + QStringLiteral("/.mwtmp-XXXXXX"));
    if (!tmp.open())
        return mw::Result<bool>::Err(QStringLiteral("无法创建临时文件: %1").arg(tmp.errorString()));
    if (tmp.write(data) != data.size() || !tmp.flush())
        return mw::Result<bool>::Err(QStringLiteral("写入失败: %1").arg(tmp.errorString()));
    if (QFileInfo::exists(path) && !QFile::remove(path))
        return mw::Result<bool>::Err(QStringLiteral("无法替换 %1").arg(path));
    tmp.setAutoRemove(false);
    if (!tmp.rename(path))
        return mw::Result<bool>::Err(
            QStringLiteral("无法落位 %1: %2").arg(path, tmp.errorString()));
    return mw::Result<bool>::Ok();
}

// 递归目录大小（字节）
inline qint64 dirSize(const QString& path)
{
    qint64 total = 0;
    QDir dir(path);
    for (const auto& e : dir.entryInfoList(
             QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks)) {
        if (e.isDir())
            total += dirSize(e.absoluteFilePath());
        else
            total += e.size();
    }
    return total;
}

} // namespace mw
