// 引擎实现 ← crates/mwine-keymap/src/engine.rs
// 数据模型复用 mw::keymap（扁平 Node 结构：字段按 kind 取用）。
#include "engine.hpp"

#include <algorithm>
#include <cmath>
#include <thread>

namespace mw::input {

using mw::keymap::Node;
using mw::keymap::TouchAction;

QPair<double, double> joystickTarget(QPair<double, double> center, double radius, quint8 dirs,
                                     Screen screen)
{
    const double dx = double((dirs & kDirRight) ? 1 : 0) - double((dirs & kDirLeft) ? 1 : 0);
    const double dy = double((dirs & kDirDown) ? 1 : 0) - double((dirs & kDirUp) ? 1 : 0);
    const double w = double(screen.first);
    const double h = double(screen.second);
    if (dx == 0.0 && dy == 0.0)
        return center;
    const double len = std::sqrt(dx * dx + dy * dy);
    const double rPx = radius * std::min(w, h);
    return { center.first + dx / len * rPx / w, center.second + dy / len * rPx / h };
}

EngineCore::EngineCore(mw::keymap::Profile profile, Screen screen, ClockFn clock)
    : m_profile(std::move(profile)), m_screen(screen), m_clock(std::move(clock))
{
    for (int i = 0; i < m_profile.nodes.size(); ++i) {
        const Node& node = m_profile.nodes[i];
        switch (node.kind) {
        case Node::Kind::Joystick:
            if (!m_joystickNode.has_value()) {
                m_joystickNode = i;
                m_joystickKeys.insert(node.up, kDirUp);
                m_joystickKeys.insert(node.down, kDirDown);
                m_joystickKeys.insert(node.left, kDirLeft);
                m_joystickKeys.insert(node.right, kDirRight);
            }
            break;
        case Node::Kind::Button:
        case Node::Kind::Skill:
        case Node::Kind::Swipe:
        case Node::Kind::Macro:
            m_keyIndex[node.key].append(i);
            break;
        case Node::Kind::Wheel:
            m_wheelNode = i;
            break;
        }
    }
}

quint64 EngineCore::allocPointer()
{
    return m_nextPointer++;
}

QPair<qint32, qint32> EngineCore::toPx(double x, double y) const
{
    const double w = double(m_screen.first);
    const double h = double(m_screen.second);
    const double px = std::min(std::max(std::round(x * w), 0.0), w - 1.0);
    const double py = std::min(std::max(std::round(y * h), 0.0), h - 1.0);
    return { qint32(px), qint32(py) };
}

double EngineCore::minSidePx() const
{
    return double(std::min(m_screen.first, m_screen.second));
}

QPair<double, double> EngineCore::nodePos(int idx) const
{
    const Node& n = m_profile.nodes[idx];
    switch (n.kind) {
    case Node::Kind::Joystick:
    case Node::Kind::Button:
    case Node::Kind::Skill:
    case Node::Kind::Swipe:
        return { n.x, n.y };
    default:
        return { 0.5, 0.5 };
    }
}

void EngineCore::emit(TouchSink* sink, TouchAction action, quint64 pointer, QPair<double, double> px)
{
    const auto [x, y] = toPx(px.first, px.second);
    sink->touch(action, pointer, x, y);
}

bool EngineCore::handleEvent(const InputEvent& ev, TouchSink* sink)
{
    switch (ev.kind) {
    case InputEvent::Kind::Key:
        return onKey(ev.key, ev.down, sink);
    case InputEvent::Kind::MouseMove: {
        std::optional<QPair<double, double>> pos;
        if (ev.hasPos)
            pos = QPair<double, double>(ev.posX, ev.posY);
        return onMouseMove(ev.dx, ev.dy, pos, sink);
    }
    case InputEvent::Kind::Scroll:
        return onScroll(ev.scrollDy, sink);
    }
    return false;
}

bool EngineCore::onKey(const QString& key, bool down, TouchSink* sink)
{
    // 总开关优先，且未激活时也吞掉（保证随时能再打开）
    if (key == m_profile.switchKey) {
        if (down)
            toggle(sink);
        return true;
    }
    if (!m_active)
        return false;
    if (m_joystickKeys.contains(key)) {
        joystickDir(m_joystickKeys.value(key), down, sink);
        return true;
    }
    const auto it = m_keyIndex.constFind(key);
    if (it == m_keyIndex.constEnd())
        return false;
    const QVector<int> indices = it.value(); // 拷贝：触发过程会改自身状态
    for (int idx : indices)
        triggerNode(idx, key, down, sink);
    return true;
}

void EngineCore::triggerNode(int idx, const QString& key, bool down, TouchSink* sink)
{
    const Node& node = m_profile.nodes[idx];
    switch (node.kind) {
    case Node::Kind::Button: {
        const QPair<double, double> pos{ node.x, node.y };
        const mw::keymap::ButtonMode mode = node.buttonMode;
        const quint32 intervalMs = node.intervalMs;
        switch (mode) {
        case mw::keymap::ButtonMode::Tap:
            if (down)
                tapNow(sink, pos);
            break;
        case mw::keymap::ButtonMode::Hold:
            if (down) {
                const quint64 p = allocPointer();
                emit(sink, TouchAction::Down, p, pos);
                m_held.insert(key, Held{ p, idx, Held::Kind::Hold });
            } else if (m_held.contains(key)) {
                const Held h = m_held.take(key);
                emit(sink, TouchAction::Up, h.pointer, pos);
            }
            break;
        case mw::keymap::ButtonMode::Repeat:
            if (down) {
                tapNow(sink, pos);
                m_timers.append({ now() + std::chrono::milliseconds(intervalMs),
                                  Timer{ Timer::Kind::RepeatTap, key, idx } });
            } else {
                // 抬键：注销连点定时器 + 若有残留按住手指（理论无）也抬起
                m_timers.erase(std::remove_if(m_timers.begin(), m_timers.end(),
                                              [&key](const QPair<TimePoint, Timer>& t) {
                                                  return t.second.kind == Timer::Kind::RepeatTap
                                                         && t.second.key == key;
                                              }),
                               m_timers.end());
            }
            break;
        case mw::keymap::ButtonMode::Toggle:
            if (down) {
                if (m_held.contains(key)) {
                    const Held h = m_held.take(key);
                    emit(sink, TouchAction::Up, h.pointer, pos);
                } else {
                    const quint64 p = allocPointer();
                    emit(sink, TouchAction::Down, p, pos);
                    m_held.insert(key, Held{ p, idx, Held::Kind::Toggle });
                }
            }
            break;
        }
        break;
    }
    case Node::Kind::Skill: {
        const QPair<double, double> pos{ node.x, node.y };
        const double dragRadius = node.dragRadius;
        const bool cancelable = node.cancelable;
        if (node.skillMode == mw::keymap::SkillMode::Quick) {
            if (down)
                tapNow(sink, pos);
        } else { // Aim
            if (down) {
                const quint64 p = allocPointer();
                emit(sink, TouchAction::Down, p, pos);
                m_held.insert(key, Held{ p, idx, Held::Kind::SkillAim });
            } else if (m_held.contains(key)) {
                const Held h = m_held.take(key);
                const double cancelPx = kCancelRadius * minSidePx();
                const auto target = aimTarget(pos, dragRadius).value_or(pos);
                const auto [sx, sy] = toPx(pos.first, pos.second);
                const auto [tx, ty] = toPx(target.first, target.second);
                const bool nearCenter =
                    std::sqrt(double((tx - sx) * (tx - sx) + (ty - sy) * (ty - sy))) < cancelPx;
                if (cancelable && nearCenter) {
                    // 拖回按钮附近 = 取消施法（手指回到按钮上再抬起）
                    emit(sink, TouchAction::Move, h.pointer, pos);
                    emit(sink, TouchAction::Up, h.pointer, pos);
                } else {
                    emit(sink, TouchAction::Up, h.pointer, target);
                }
            }
        }
        break;
    }
    case Node::Kind::Swipe:
        if (down) {
            swipeNow(sink, { node.x, node.y }, { node.x2, node.y2 }, node.durationMs,
                     std::nullopt);
        }
        break;
    case Node::Kind::Macro:
        if (down && !node.steps.isEmpty()) {
            const quint64 run = m_nextMacroRun++;
            m_macros.insert(run, MacroRun{ idx, 0, std::nullopt });
            // 立即执行第一步
            advanceMacro(run, sink);
        }
        break;
    case Node::Kind::Wheel:
    case Node::Kind::Joystick:
        break;
    }
}

std::optional<QPair<double, double>> EngineCore::aimTarget(QPair<double, double> pos,
                                                           double dragRadius) const
{
    if (!m_mousePos.has_value())
        return std::nullopt;
    const auto [mx, my] = *m_mousePos;
    const double dx = mx - pos.first;
    const double dy = my - pos.second;
    // 归一化坐标 x/y 轴比例不同，先换算到像素空间估长度
    const double w = double(m_screen.first);
    const double h = double(m_screen.second);
    const double len = std::sqrt(dx * w * dx * w + dy * h * dy * h);
    if (len < 1.0)
        return std::nullopt;
    const double drag = dragRadius * minSidePx();
    const double scale = len > drag ? drag / len : 1.0;
    return QPair<double, double>(pos.first + dx * scale, pos.second + dy * scale);
}

void EngineCore::tapNow(TouchSink* sink, QPair<double, double> pos)
{
    const quint64 p = allocPointer();
    emit(sink, TouchAction::Down, p, pos);
    emit(sink, TouchAction::Up, p, pos);
}

void EngineCore::swipeNow(TouchSink* sink, QPair<double, double> from, QPair<double, double> to,
                          quint32 durationMs, std::optional<quint64> boundPointer)
{
    const quint64 p = boundPointer.value_or(allocPointer());
    if (!boundPointer.has_value())
        emit(sink, TouchAction::Down, p, from);
    const quint32 total = quint32(std::max<quint64>(quint64(durationMs) / kSwipeFrameMs, 2));
    const TimePoint t0 = now();
    for (quint32 i = 1; i <= total; ++i) {
        Timer t;
        t.kind = Timer::Kind::SwipeFrame;
        t.pointer = p;
        t.from = from;
        t.to = to;
        t.idx = i;
        t.total = total;
        t.last = i == total;
        m_timers.append({ t0 + std::chrono::milliseconds(quint64(i) * kSwipeFrameMs), t });
    }
}

void EngineCore::joystickDir(quint8 bit, bool down, TouchSink* sink)
{
    if (!m_joystickNode.has_value())
        return;
    const Node& jn = m_profile.nodes[*m_joystickNode];
    const QPair<double, double> center{ jn.x, jn.y };
    const double radius = jn.radius;

    if (down) {
        const quint8 dirs = m_joystick.has_value() ? (m_joystick->dirs | bit) : bit;
        const QPair<double, double> pos = joystickTarget(center, radius, dirs, m_screen);
        if (!m_joystick.has_value()) {
            const quint64 p = allocPointer();
            m_joystick = JoyState{ p, dirs, pos };
            // 真实手感：先在中心按下，再推出到目标方向
            emit(sink, TouchAction::Down, p, center);
            emit(sink, TouchAction::Move, p, pos);
        } else {
            const quint64 p = m_joystick->pointer;
            m_joystick->dirs = dirs;
            m_joystick->px = pos;
            emit(sink, TouchAction::Move, p, pos);
        }
    } else if (m_joystick.has_value()) {
        const quint8 dirs = m_joystick->dirs & ~bit;
        const quint64 pointer = m_joystick->pointer;
        if (dirs == 0) {
            const QPair<double, double> px = m_joystick->px;
            m_joystick.reset();
            emit(sink, TouchAction::Up, pointer, px);
        } else {
            const QPair<double, double> pos = joystickTarget(center, radius, dirs, m_screen);
            m_joystick->dirs = dirs;
            m_joystick->px = pos;
            emit(sink, TouchAction::Move, pointer, pos);
        }
    }
}

bool EngineCore::onMouseMove(double dx, double dy, std::optional<QPair<double, double>> pos,
                             TouchSink* sink)
{
    if (pos.has_value())
        m_mousePos = pos;
    if (!m_active)
        return false;
    // 方向技能瞄准更新
    for (auto it = m_held.constBegin(); it != m_held.constEnd(); ++it) {
        const Held& h = it.value();
        if (h.kind != Held::Kind::SkillAim)
            continue;
        const Node& node = m_profile.nodes[h.node];
        if (node.kind != Node::Kind::Skill)
            continue;
        const auto target =
            aimTarget({ node.x, node.y }, node.dragRadius).value_or(QPair<double, double>(node.x, node.y));
        emit(sink, TouchAction::Move, h.pointer, target);
    }
    // FPS 视角拖拽
    if (m_profile.mouseMode == mw::keymap::MouseMode::Capture) {
        lookMove(dx, dy, sink);
        return true;
    }
    return false;
}

void EngineCore::lookMove(double dx, double dy, TouchSink* sink)
{
    if (!m_profile.hasLook)
        return;
    const mw::keymap::LookConfig& cfg = m_profile.look;
    const double w = double(m_screen.first);
    const double h = double(m_screen.second);
    if (!m_look.pointer.has_value()) {
        const quint64 p = allocPointer();
        m_look.pos = { cfg.x, cfg.y };
        emit(sink, TouchAction::Down, p, m_look.pos);
        m_look.pointer = p;
    }
    const double nx = m_look.pos.first + dx * cfg.sensX / w;
    const double ny = m_look.pos.second + dy * cfg.sensY / h;
    const std::array<double, 4> b = cfg.boundsArr();
    if (nx < b[0] || nx > b[2] || ny < b[1] || ny > b[3]) {
        // 越界回绕：抬指 → 锚点重新落指（视角已转过的量保留）
        const quint64 p = *m_look.pointer;
        emit(sink, TouchAction::Up, p, m_look.pos);
        m_look.pos = { cfg.x, cfg.y };
        emit(sink, TouchAction::Down, p, m_look.pos);
        return;
    }
    m_look.pos = { nx, ny };
    emit(sink, TouchAction::Move, *m_look.pointer, m_look.pos);
}

bool EngineCore::onScroll(double dy, TouchSink* sink)
{
    if (!m_active)
        return false;
    const bool consume = captured() || m_wheelNode.has_value();
    if (!consume)
        return false;
    const TimePoint t = now();
    if (m_lastScroll.has_value()) {
        if (std::chrono::duration_cast<std::chrono::milliseconds>(t - *m_lastScroll).count()
            < kScrollThrottleMs)
            return true;
    }
    m_lastScroll = t;
    if (m_wheelNode.has_value()) {
        const Node& node = m_profile.nodes[*m_wheelNode];
        if (dy < 0.0)
            tapNow(sink, { node.upX, node.upY });
        else if (dy > 0.0)
            tapNow(sink, { node.downX, node.downY });
    }
    return true;
}

bool EngineCore::toggle(TouchSink* sink)
{
    m_active = !m_active;
    if (!m_active) {
        // 进入捕获：视角手指待首次移动时落指（此处不预按，
        // 避免开关瞬间误触视角拖拽）——active 分支无动作
        releaseAll(sink);
    }
    return m_active;
}

void EngineCore::releaseAll(TouchSink* sink)
{
    if (m_joystick.has_value()) {
        emit(sink, TouchAction::Up, m_joystick->pointer, m_joystick->px);
        m_joystick.reset();
    }
    const QVector<Held> helds = m_held.values();
    for (const Held& h : helds)
        emit(sink, TouchAction::Up, h.pointer, nodePos(h.node));
    m_held.clear();
    if (m_look.pointer.has_value()) {
        emit(sink, TouchAction::Up, *m_look.pointer, m_look.pos);
        m_look.pointer.reset();
    }
    // 滑动未完成帧直接在当前位收尾（帧任务里含 last 的 Up）
    QVector<quint64> pending;
    for (const auto& [t, timer] : m_timers) {
        if (timer.kind == Timer::Kind::SwipeFrame)
            pending.append(timer.pointer);
    }
    m_timers.clear();
    m_macros.clear();
    for (quint64 p : pending)
        sink->touch(TouchAction::Up, p, 0, 0);
}

void EngineCore::runDue(TimePoint nowTs, TouchSink* sink)
{
    for (;;) {
        bool progressed = false;
        QVector<QPair<TimePoint, Timer>> due;
        QVector<QPair<TimePoint, Timer>> rest;
        for (auto& item : m_timers) {
            if (item.first <= nowTs)
                due.append(std::move(item));
            else
                rest.append(std::move(item));
        }
        m_timers = std::move(rest);
        // 按到期时间排序保证帧顺序
        std::sort(due.begin(), due.end(),
                  [](const QPair<TimePoint, Timer>& a, const QPair<TimePoint, Timer>& b) {
                      return a.first < b.first;
                  });
        for (auto& [dueAt, timer] : due) {
            (void)dueAt;
            progressed = true;
            switch (timer.kind) {
            case Timer::Kind::RepeatTap: {
                if (timer.node < 0 || timer.node >= m_profile.nodes.size())
                    continue;
                const Node& node = m_profile.nodes[timer.node];
                if (node.kind != Node::Kind::Button
                    || node.buttonMode != mw::keymap::ButtonMode::Repeat)
                    continue;
                const QPair<double, double> pos{ node.x, node.y };
                const std::chrono::milliseconds interval(node.intervalMs);
                tapNow(sink, pos);
                Timer next;
                next.kind = Timer::Kind::RepeatTap;
                next.key = timer.key;
                next.node = timer.node;
                m_timers.append({ nowTs + interval, next });
                break;
            }
            case Timer::Kind::MacroStep:
                if (m_macros.contains(timer.run))
                    advanceMacro(timer.run, sink);
                break;
            case Timer::Kind::SwipeFrame: {
                const double t = double(timer.idx) / double(timer.total);
                const QPair<double, double> pos(
                    timer.from.first + (timer.to.first - timer.from.first) * t,
                    timer.from.second + (timer.to.second - timer.from.second) * t);
                emit(sink, TouchAction::Move, timer.pointer, pos);
                if (timer.last)
                    emit(sink, TouchAction::Up, timer.pointer, timer.to);
                break;
            }
            }
        }
        if (!progressed)
            break;
    }
}

void EngineCore::advanceMacro(quint64 run, TouchSink* sink)
{
    const auto it = m_macros.find(run);
    if (it == m_macros.end())
        return;
    const int node = it.value().node;
    const int stepI = it.value().step;
    std::optional<quint64> heldPointer = it.value().pointer;
    if (node < 0 || node >= m_profile.nodes.size()) {
        m_macros.remove(run);
        return;
    }
    const Node& n = m_profile.nodes[node];
    if (n.kind != Node::Kind::Macro) {
        m_macros.remove(run);
        return;
    }
    const QVector<mw::keymap::MacroStep> steps = n.steps; // 拷贝：过程会改 m_macros
    if (stepI < 0 || stepI >= steps.size()) {
        // 序列执行完毕：若 TouchDown 未配对 Up，兜底抬起
        if (heldPointer.has_value()) {
            sink->touch(TouchAction::Up, *heldPointer, 0, 0);
            if (m_macros.contains(run))
                m_macros[run].pointer.reset();
        }
        m_macros.remove(run);
        return;
    }
    const mw::keymap::MacroStep step = steps[stepI];
    std::chrono::milliseconds wait(0);
    switch (step.action) {
    case mw::keymap::MacroStep::Action::Tap:
        tapNow(sink, { step.x, step.y });
        wait = std::chrono::milliseconds(step.delayMs);
        break;
    case mw::keymap::MacroStep::Action::TouchDown: {
        const quint64 p = allocPointer();
        emit(sink, TouchAction::Down, p, { step.x, step.y });
        if (m_macros.contains(run))
            m_macros[run].pointer = p;
        wait = std::chrono::milliseconds(step.delayMs);
        break;
    }
    case mw::keymap::MacroStep::Action::TouchUp:
        if (heldPointer.has_value()) {
            const QPair<double, double> pos = nodePos(node); // 近似位置，Up 坐标不敏感
            emit(sink, TouchAction::Up, *heldPointer, pos);
        }
        if (m_macros.contains(run))
            m_macros[run].pointer.reset();
        wait = std::chrono::milliseconds(step.delayMs);
        break;
    case mw::keymap::MacroStep::Action::Swipe:
        // 宏内滑动：复用持有的手指（若有），否则独立手势
        swipeNow(sink, { step.x, step.y }, { step.x2, step.y2 }, step.durationMs, heldPointer);
        wait = std::chrono::milliseconds(quint64(step.durationMs) + step.delayMs);
        break;
    case mw::keymap::MacroStep::Action::Delay:
        wait = std::chrono::milliseconds(step.ms);
        break;
    }
    if (m_macros.contains(run)) {
        m_macros[run].step = stepI + 1;
        Timer t;
        t.kind = Timer::Kind::MacroStep;
        t.run = run;
        m_timers.append({ now() + wait, t });
    }
}

std::optional<EngineCore::TimePoint> EngineCore::nextDeadline() const
{
    if (m_timers.isEmpty())
        return std::nullopt;
    TimePoint earliest = m_timers.first().first;
    for (const auto& [t, timer] : m_timers) {
        (void)timer;
        if (t < earliest)
            earliest = t;
    }
    return earliest;
}

// ---- Engine：状态机 + 后台定时器线程 ----

Engine::Engine(std::unique_ptr<EngineCore> core, SharedSink sink)
    : m_core(std::move(core)), m_sink(std::move(sink))
{
    m_worker = std::thread([this] {
        for (;;) {
            if (m_stop.load(std::memory_order_relaxed))
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            std::lock_guard<std::mutex> coreLock(m_coreMutex);
            std::lock_guard<std::mutex> sinkLock(*m_sink.mutex);
            m_core->runDue(EngineCore::Clock::now(), m_sink.sink.get());
        }
    });
}

Engine::~Engine()
{
    shutdown();
}

std::shared_ptr<Engine> Engine::create(mw::keymap::Profile profile, Screen screen, SharedSink sink)
{
    return std::shared_ptr<Engine>(
        new Engine(std::make_unique<EngineCore>(std::move(profile), screen), std::move(sink)));
}

bool Engine::handleEvent(const InputEvent& ev)
{
    std::lock_guard<std::mutex> coreLock(m_coreMutex);
    std::lock_guard<std::mutex> sinkLock(*m_sink.mutex);
    return m_core->handleEvent(ev, m_sink.sink.get());
}

bool Engine::active() const
{
    std::lock_guard<std::mutex> lock(m_coreMutex);
    return m_core->active();
}

bool Engine::captured() const
{
    std::lock_guard<std::mutex> lock(m_coreMutex);
    return m_core->captured();
}

QString Engine::switchKey() const
{
    std::lock_guard<std::mutex> lock(m_coreMutex);
    return m_core->profile().switchKey;
}

void Engine::releaseFingers()
{
    std::lock_guard<std::mutex> coreLock(m_coreMutex);
    std::lock_guard<std::mutex> sinkLock(*m_sink.mutex);
    m_core->releaseAll(m_sink.sink.get());
}

mw::keymap::Profile Engine::profileSnapshot() const
{
    std::lock_guard<std::mutex> lock(m_coreMutex);
    return m_core->profile();
}

Screen Engine::screen() const
{
    std::lock_guard<std::mutex> lock(m_coreMutex);
    return m_core->screen();
}

void Engine::shutdown()
{
    m_stop.store(true, std::memory_order_seq_cst);
    if (m_worker.joinable())
        m_worker.join();
    std::lock_guard<std::mutex> coreLock(m_coreMutex);
    std::lock_guard<std::mutex> sinkLock(*m_sink.mutex);
    m_core->releaseAll(m_sink.sink.get());
}

} // namespace mw::input
