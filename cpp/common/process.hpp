// 同步子进程：QProcess 封装，阻塞至完成或超时（工作线程内使用）。
#pragma once

#include <QProcess>
#include <QStringList>

#include "result.hpp"

namespace mw {

struct ProcessResult {
    int exitCode = -1;
    QByteArray stdOut;
    QByteArray stdErr;
    QString error; // 启动失败等（空 = 进程跑完了）

    bool ok() const { return error.isEmpty() && exitCode == 0; }
    QString stdOutStr() const { return QString::fromUtf8(stdOut); }
    QString stdErrStr() const { return QString::fromUtf8(stdErr); }
    QString stdErrTail() const // 末行（错误提示用）
    {
        const auto lines = stdErrStr().split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        return lines.isEmpty() ? QString() : lines.last().trimmed();
    }
};

inline ProcessResult runProcess(const QString& program, const QStringList& args,
                                int timeoutMs = 60000)
{
    QProcess p;
    p.setProgram(program);
    p.setArguments(args);
    p.start();
    if (!p.waitForStarted(5000))
        return { -1, {}, {}, QStringLiteral("无法启动 %1: %2").arg(program, p.errorString()) };
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(2000);
        return { -1, {}, {}, QStringLiteral("%1 超时（%2ms）").arg(program).arg(timeoutMs) };
    }
    return { p.exitCode(), p.readAllStandardOutput(), p.readAllStandardError(), {} };
}

} // namespace mw
