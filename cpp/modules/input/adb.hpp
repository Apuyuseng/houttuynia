// adb 注入后端（低速备选）：`adb shell input`，无 server 依赖，
// 任何显示器均可（`--display`，Android 10+），单次往返 ~50-200ms。
// ← crates/mwine-input/src/adb.rs
//
// 能力取舍（与 scrcpy 后端的差异，组装时须知）：
// - `input` 无法分离按下/抬起 → 触摸与按键均为「Up 完成式」：
//   Down 记录起点，Up 才真正下发 tap / swipe / keyevent
// - 不支持多指并发（第二个指针 Down 返回 errUnsupported）
// - 按住 >500ms 后 Up → `keyevent --longpress`
#pragma once

#include <QHash>
#include <QPair>
#include <QString>

#include <chrono>
#include <memory>
#include <optional>

#include "client.hpp"
#include "input.hpp"
#include "result.hpp"

namespace mw::input {

/// 单引号 shell 转义（adb shell 经 sh 解释）。
QString shellQuote(const QString& s);

/// `input text` 参数编码：空格 → `%s`（input 内部约定），再整体 shell 引号包裹。
QString textArg(const QString& text);

/// 显示器前缀：0（主屏）省略参数以兼容老 Android；其余 `--display <id> `。
QString displayPrefix(quint32 displayId);

/// 待完成触摸：Down 已记录，等 Up 决定 tap 还是 swipe。
struct PendingTouch {
    QPair<qint32, qint32> down{0, 0};
    QPair<qint32, qint32> last{0, 0};
    std::chrono::steady_clock::time_point start;
};

/// Up 时刻的触摸计划（纯状态机输出，便于无设备测试）。
struct TouchPlan {
    enum class Kind { Tap, Swipe, None };
    Kind kind = Kind::None;
    qint32 x = 0, y = 0;          // Tap
    qint32 x1 = 0, y1 = 0;        // Swipe 起点
    qint32 x2 = 0, y2 = 0;        // Swipe 终点
    quint64 durationMs = 0;       // Swipe 时长

    static TouchPlan tap(qint32 x, qint32 y)
    {
        TouchPlan p;
        p.kind = Kind::Tap;
        p.x = x;
        p.y = y;
        return p;
    }
    static TouchPlan swipe(qint32 x1, qint32 y1, qint32 x2, qint32 y2, quint64 durationMs)
    {
        TouchPlan p;
        p.kind = Kind::Swipe;
        p.x1 = x1;
        p.y1 = y1;
        p.x2 = x2;
        p.y2 = y2;
        p.durationMs = durationMs;
        return p;
    }
};

/// 由按住时长与位移决定 Up 计划（planTouchUp 的纯函数形态，now 可注入）。
TouchPlan planTouchUp(const PendingTouch& p,
                      std::chrono::steady_clock::time_point now
                      = std::chrono::steady_clock::now());

/// 滚轮 → 滑动映射：scrcpy 语义 h/v ∈ [-1,1]（与 AXIS_VSCROLL 同号，
/// 正 v = 滚轮向上 = 视图滚向顶部 = 手指向下滑），距离固定 DISTANCE px。
inline constexpr qint32 kScrollDistancePx = 240;
inline constexpr quint64 kScrollDurationMs = 250;

QPair<qint32, qint32> scrollTarget(qint32 x, qint32 y, float h, float v);

/// adb `input` 注入后端。
class AdbInjector : public InputInjector {
public:
    AdbInjector(mw::adb::AdbClient adb, QString serial)
        : m_adb(std::move(adb)), m_serial(std::move(serial))
    {
    }

    AdbInjector* withTimeout(std::chrono::milliseconds timeout)
    {
        m_timeoutMs = int(timeout.count());
        return this;
    }

    mw::Result<bool> injectTouch(quint32 displayId, TouchAction action, quint64 pointerId,
                                 qint32 x, qint32 y) override;
    mw::Result<bool> injectKey(const AndroidKeycode& keycode, KeyAction action) override;
    mw::Result<bool> injectText(const QString& text) override;
    mw::Result<bool> injectScroll(quint32 displayId, qint32 x, qint32 y, float h,
                                  float v) override;

private:
    mw::Result<bool> runInput(const QString& args);

    mw::adb::AdbClient m_adb;
    QString m_serial;
    int m_timeoutMs = 5000;
    std::chrono::milliseconds m_longPressMs{500};
    std::optional<QPair<quint64, PendingTouch>> m_pendingTouch;
    QHash<qint32, std::chrono::steady_clock::time_point> m_pendingKeys;
};

} // namespace mw::input
