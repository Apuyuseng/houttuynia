#include "state.hpp"

namespace mw::daemon {

QString AppState::currentSerial()
{
    QMutexLocker l(&vmMu);
    return vmSerial;
}

bool AppState::vmRunning()
{
    const auto ds = adb.devices();
    if (!ds)
        return false;
    for (const auto& d : ds.value())
        if (d.state == QStringLiteral("device") && d.serial.startsWith(QStringLiteral("emulator-")))
            return true;
    return false;
}

void AppState::setVmError(const QString& e)
{
    QMutexLocker l(&vmMu);
    vmError = e;
}

QString AppState::vmErrorLocked()
{
    QMutexLocker l(&vmMu);
    return vmError;
}

void AppState::adoptSerial(const QString& s)
{
    QMutexLocker l(&vmMu);
    vmSerial = s;
}

} // namespace mw::daemon
