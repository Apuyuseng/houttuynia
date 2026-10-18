// mw_input 自测：纯逻辑（参数协商 / 坐标变换 / engine 假时钟确定性调度）。
// 注意：不用 <cassert>（Release 下 NDEBUG 会使 assert 失效），自定义 CHECK。
#include <QCoreApplication>
#include <QDebug>
#include <QString>
#include <QVector>
#include <QtGlobal>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>

#include "adb.hpp"
#include "console.hpp"
#include "engine.hpp"
#include "event_tap.hpp"
#include "input.hpp"
#include "scrcpy.hpp"

static int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            qCritical().noquote() << QStringLiteral("FAIL %1:%2 %3")            \
                                         .arg(QString::fromLatin1(__FILE__),   \
                                              QString::number(__LINE__),        \
                                              QString::fromLatin1(#cond));      \
        }                                                                      \
    } while (0)

using namespace mw::input;
using KTouch = mw::keymap::TouchAction; // 与 mw::input::TouchAction 消歧
using KNode = mw::keymap::Node;

static bool near(double a, double b, double eps = 1e-9) { return qAbs(a - b) < eps; }

// ---- 假时钟：确定性调度 ----

namespace {

using Clock = EngineCore::Clock;

class FakeClock {
public:
    EngineCore::ClockFn fn() { return [this] { return m_now; }; }
    void advanceMs(quint64 ms)
    {
        m_now += std::chrono::milliseconds(ms);
    }
    Clock::time_point now() const { return m_now; }

private:
    Clock::time_point m_now = Clock::time_point(std::chrono::milliseconds(1000000));
};

/// 记录型 sink：(action, pointer, x, y)
struct Rec : TouchSink {
    struct Ev {
        KTouch action;
        quint64 pointer;
        qint32 x, y;
    };
    QVector<Ev> events;
    void touch(mw::keymap::TouchAction action, quint64 pointer, qint32 x, qint32 y) override
    {
        events.append(Ev{ action, pointer, x, y });
    }
    bool eq(int i, KTouch a, quint64 p, qint32 x, qint32 y) const
    {
        if (i < 0 || i >= events.size())
            return false;
        const Ev& e = events[i];
        return e.action == a && e.pointer == p && e.x == x && e.y == y;
    }
};

constexpr quint16 kW = 1000, kH = 500; // 屏幕与 Rust 侧测试一致

mw::keymap::Profile makeProfile()
{
    mw::keymap::Profile p;
    p.switchKey = QStringLiteral("Grave");
    p.mouseMode = mw::keymap::MouseMode::Free;
    return p;
}

KNode buttonNode(const QString& key, double x, double y, mw::keymap::ButtonMode mode,
                 quint32 intervalMs = 120)
{
    KNode n;
    n.kind = KNode::Kind::Button;
    n.key = key;
    n.x = x;
    n.y = y;
    n.buttonMode = mode;
    n.intervalMs = intervalMs;
    return n;
}

bool keyPress(EngineCore& ec, Rec& rec, const QString& k, bool down)
{
    return ec.handleEvent(InputEvent::keyEvent(k, down), &rec);
}

} // namespace

// ---- 摇杆目标点（纯函数）----

static void testJoystickTarget()
{
    // 短边 500、radius 0.2 → 100px：垂直位移 0.2（100/500），水平 0.1（100/1000）
    const auto up = joystickTarget({ 0.2, 0.8 }, 0.2, kDirUp, { kW, kH });
    CHECK(near(up.first, 0.2) && near(up.second, 0.6));
    const auto right = joystickTarget({ 0.2, 0.8 }, 0.2, kDirRight, { kW, kH });
    CHECK(near(right.first, 0.3) && near(right.second, 0.8));
    // 无方向 → 中心
    const auto none = joystickTarget({ 0.2, 0.8 }, 0.2, 0, { kW, kH });
    CHECK(near(none.first, 0.2) && near(none.second, 0.8));
}

static void testJoystickFullCycle()
{
    auto p = makeProfile();
    {
        KNode j;
        j.kind = KNode::Kind::Joystick;
        j.x = 0.2;
        j.y = 0.8;
        j.radius = 0.2;
        p.nodes.append(j);
    }
    FakeClock clock;
    EngineCore ec(std::move(p), { kW, kH }, clock.fn());
    Rec rec;
    // 按 W：Down 在中心 (200,400)，Move 到 (200,300)
    CHECK(keyPress(ec, rec, QStringLiteral("W"), true));
    CHECK(rec.events.size() == 2);
    CHECK(rec.eq(0, KTouch::Down, 1001, 200, 400));
    CHECK(rec.eq(1, KTouch::Move, 1001, 200, 300));
    // 加按 D：对角 center + (0.707,-0.707)*100 → (271, 329)
    CHECK(keyPress(ec, rec, QStringLiteral("D"), true));
    CHECK(rec.eq(2, KTouch::Move, 1001, 271, 329));
    // 松 W：回到纯右 (300, 400)
    CHECK(keyPress(ec, rec, QStringLiteral("W"), false));
    CHECK(rec.eq(3, KTouch::Move, 1001, 300, 400));
    // 松 D：抬起
    CHECK(keyPress(ec, rec, QStringLiteral("D"), false));
    CHECK(rec.eq(4, KTouch::Up, 1001, 300, 400));
    CHECK(rec.events.size() == 5);
}

static void testButtonModes()
{
    auto p = makeProfile();
    p.nodes.append(buttonNode(QStringLiteral("J"), 0.1, 0.1, mw::keymap::ButtonMode::Tap));
    p.nodes.append(buttonNode(QStringLiteral("K"), 0.2, 0.2, mw::keymap::ButtonMode::Hold));
    p.nodes.append(
        buttonNode(QStringLiteral("L"), 0.3, 0.3, mw::keymap::ButtonMode::Repeat, 100));
    p.nodes.append(buttonNode(QStringLiteral("T"), 0.4, 0.4, mw::keymap::ButtonMode::Toggle));
    FakeClock clock;
    EngineCore ec(std::move(p), { kW, kH }, clock.fn());
    Rec rec;

    // tap：即刻 Down+Up
    keyPress(ec, rec, QStringLiteral("J"), true);
    CHECK(rec.events.size() == 2);
    // hold：Down 保持，Up 配对
    keyPress(ec, rec, QStringLiteral("K"), true);
    CHECK(rec.events.last().action == KTouch::Down);
    const quint64 holdPtr = rec.events.last().pointer;
    keyPress(ec, rec, QStringLiteral("K"), false);
    CHECK(rec.eq(rec.events.size() - 1, KTouch::Up, holdPtr, 200, 100));
    // repeat：按下立即一次，100ms 后再一次
    int base = rec.events.size();
    keyPress(ec, rec, QStringLiteral("L"), true);
    CHECK(rec.events.size() == base + 2);
    clock.advanceMs(150);
    ec.runDue(clock.now(), &rec);
    CHECK(rec.events.size() == base + 4);
    // 抬键后不再连点
    base = rec.events.size();
    keyPress(ec, rec, QStringLiteral("L"), false);
    clock.advanceMs(150);
    ec.runDue(clock.now(), &rec);
    CHECK(rec.events.size() == base);
    // toggle：按下 Down，再按（仍按 Down 物理语义）抬起
    keyPress(ec, rec, QStringLiteral("T"), true);
    CHECK(rec.events.last().action == KTouch::Down);
    keyPress(ec, rec, QStringLiteral("T"), true);
    CHECK(rec.events.last().action == KTouch::Up);
}

static void testSkillAimCastAndCancel()
{
    auto p = makeProfile(); // mouseMode = Free
    {
        KNode s;
        s.kind = KNode::Kind::Skill;
        s.key = QStringLiteral("Q");
        s.x = 0.5;
        s.y = 0.5;
        s.skillMode = mw::keymap::SkillMode::Aim;
        s.dragRadius = 0.2;
        s.cancelable = true;
        p.nodes.append(s);
    }
    FakeClock clock;
    EngineCore ec(std::move(p), { kW, kH }, clock.fn());
    Rec rec;

    keyPress(ec, rec, QStringLiteral("Q"), true);
    CHECK(rec.events.size() == 1 && rec.eq(0, KTouch::Down, 1001, 500, 250));
    // 鼠标移到 (0.9,0.1)：向量 (400,-200) 长 447 > 拖距 100 → 截断 → (589, 205)
    ec.handleEvent(InputEvent::mouseMove(0.0, 0.0, true, 0.9, 0.1), &rec);
    CHECK(rec.eq(rec.events.size() - 1, KTouch::Move, 1001, 589, 205));
    // 远处松手 → 在瞄准点 Up（施放）
    keyPress(ec, rec, QStringLiteral("Q"), false);
    CHECK(rec.eq(rec.events.size() - 1, KTouch::Up, 1001, 589, 205));
    // 再来一次：鼠标挪回按钮附近（< 取消半径 30px）→ Move 回按钮 + Up（取消施法）
    keyPress(ec, rec, QStringLiteral("Q"), true);
    ec.handleEvent(InputEvent::mouseMove(0.0, 0.0, true, 0.52, 0.51), &rec);
    const int n = rec.events.size();
    keyPress(ec, rec, QStringLiteral("Q"), false);
    CHECK(rec.eq(n, KTouch::Move, 1002, 500, 250));
    CHECK(rec.eq(n + 1, KTouch::Up, 1002, 500, 250));
}

static void testLookCaptureAndWraparound()
{
    auto p = makeProfile();
    p.mouseMode = mw::keymap::MouseMode::Capture;
    p.hasLook = true;
    p.look.x = 0.6;
    p.look.y = 0.3;
    p.look.sensX = 3.0;
    p.look.sensY = 2.0;
    p.look.hasBounds = true;
    p.look.bounds = { 0.3, 0.05, 0.98, 0.95 };
    p.nodes.append(
        buttonNode(QStringLiteral("MouseLeft"), 0.86, 0.72, mw::keymap::ButtonMode::Hold));
    FakeClock clock;
    EngineCore ec(std::move(p), { kW, kH }, clock.fn());
    Rec rec;

    // 移动鼠标：落指 + Move
    const bool consumed = ec.handleEvent(InputEvent::mouseMove(10.0, 5.0, true, 0.6, 0.3), &rec);
    CHECK(consumed);
    CHECK(rec.eq(0, KTouch::Down, 1001, 600, 150));
    // x += 10*3/1000 = 0.63 → 630；y += 5*2/500 = 0.32 → 160
    CHECK(rec.eq(1, KTouch::Move, 1001, 630, 160));
    // 大增量越界 → 当前位置 Up + 回锚点 Down（回绕）
    ec.handleEvent(InputEvent::mouseMove(3000.0, 0.0, true, 0.9, 0.3), &rec);
    const int n = rec.events.size();
    CHECK(rec.eq(n - 2, KTouch::Up, 1001, 630, 160));
    CHECK(rec.eq(n - 1, KTouch::Down, 1001, 600, 150));
    // 开火键（capture 模式下鼠标键被吞）
    CHECK(keyPress(ec, rec, QStringLiteral("MouseLeft"), true));
    CHECK(rec.events.last().action == KTouch::Down);
}

static void testSwitchKeyTogglesAndReleases()
{
    auto p = makeProfile();
    p.nodes.append(buttonNode(QStringLiteral("K"), 0.2, 0.2, mw::keymap::ButtonMode::Hold));
    FakeClock clock;
    EngineCore ec(std::move(p), { kW, kH }, clock.fn());
    Rec rec;

    keyPress(ec, rec, QStringLiteral("K"), true);
    CHECK(rec.events.size() == 1);
    // 总开关关闭：释放手指
    CHECK(keyPress(ec, rec, QStringLiteral("Grave"), true));
    CHECK(!ec.active());
    CHECK(rec.events.size() == 2);
    CHECK(rec.events[1].action == KTouch::Up);
    // 关闭后按键放行、不注入
    CHECK(!keyPress(ec, rec, QStringLiteral("K"), true));
    CHECK(rec.events.size() == 2);
    // 再开
    CHECK(keyPress(ec, rec, QStringLiteral("Grave"), true));
    CHECK(ec.active());
    CHECK(keyPress(ec, rec, QStringLiteral("K"), true));
    CHECK(rec.events.size() == 3);
}

static void testSwipeInterpolates()
{
    auto p = makeProfile();
    {
        KNode s;
        s.kind = KNode::Kind::Swipe;
        s.key = QStringLiteral("X");
        s.x = 0.1;
        s.y = 0.5;
        s.x2 = 0.3;
        s.y2 = 0.5;
        s.durationMs = 64;
        p.nodes.append(s);
    }
    FakeClock clock;
    EngineCore ec(std::move(p), { kW, kH }, clock.fn());
    Rec rec;

    keyPress(ec, rec, QStringLiteral("X"), true);
    CHECK(rec.eq(0, KTouch::Down, 1001, 100, 250));
    clock.advanceMs(200);
    ec.runDue(clock.now(), &rec);
    // 帧序列以 Up 结束，终点 (300,250)
    CHECK(rec.eq(rec.events.size() - 1, KTouch::Up, 1001, 300, 250));
    // 中间帧 x 单调推进
    bool monotonic = true;
    for (int i = 2; i < rec.events.size() - 1; ++i)
        monotonic = monotonic && rec.events[i - 1].x < rec.events[i].x;
    CHECK(monotonic);
}

static void testWheelScrollTaps()
{
    auto p = makeProfile();
    p.mouseMode = mw::keymap::MouseMode::Capture;
    {
        KNode w;
        w.kind = KNode::Kind::Wheel;
        w.upX = 0.87;
        w.upY = 0.62;
        w.downX = 0.87;
        w.downY = 0.68;
        p.nodes.append(w);
    }
    FakeClock clock;
    EngineCore ec(std::move(p), { kW, kH }, clock.fn());
    Rec rec;

    CHECK(ec.handleEvent(InputEvent::scroll(-12.0), &rec));
    const int n = rec.events.size();
    CHECK(rec.eq(n - 2, KTouch::Down, 1001, 870, 310)); // 上滚 → tap up 位置
    CHECK(rec.eq(n - 1, KTouch::Up, 1001, 870, 310));
    // 节流期内忽略（假时钟未推进）
    CHECK(ec.handleEvent(InputEvent::scroll(-12.0), &rec));
    CHECK(rec.events.size() == n);
    // 过节流窗口后再滚（向下 → down 位置）
    clock.advanceMs(100);
    CHECK(ec.handleEvent(InputEvent::scroll(12.0), &rec));
    CHECK(rec.eq(rec.events.size() - 1, KTouch::Up, 1002, 870, 340));
}

static void testMacroSequence()
{
    auto p = makeProfile();
    {
        KNode m;
        m.kind = KNode::Kind::Macro;
        m.key = QStringLiteral("F1");
        mw::keymap::MacroStep s1;
        s1.action = mw::keymap::MacroStep::Action::Tap;
        s1.x = 0.1;
        s1.y = 0.1;
        s1.delayMs = 50;
        mw::keymap::MacroStep s2;
        s2.action = mw::keymap::MacroStep::Action::TouchDown;
        s2.x = 0.2;
        s2.y = 0.2;
        s2.delayMs = 0;
        mw::keymap::MacroStep s3;
        s3.action = mw::keymap::MacroStep::Action::Delay;
        s3.ms = 30;
        mw::keymap::MacroStep s4;
        s4.action = mw::keymap::MacroStep::Action::TouchUp;
        s4.delayMs = 0;
        m.steps = { s1, s2, s3, s4 };
        p.nodes.append(m);
    }
    FakeClock clock;
    EngineCore ec(std::move(p), { kW, kH }, clock.fn());
    Rec rec;

    keyPress(ec, rec, QStringLiteral("F1"), true);
    // 第一步立即执行：tap (100,50)
    CHECK(rec.eq(0, KTouch::Down, 1001, 100, 50));
    CHECK(rec.eq(1, KTouch::Up, 1001, 100, 50));
    clock.advanceMs(10);
    ec.runDue(clock.now(), &rec);
    CHECK(rec.events.size() == 2);
    clock.advanceMs(50); // t=60：> tap 后 50ms 等待
    ec.runDue(clock.now(), &rec);
    // touchDown (200,100)；随后的 delay(30) 定时器在 t=90
    CHECK(rec.eq(2, KTouch::Down, 1002, 200, 100));
    CHECK(rec.events.size() == 3);
    clock.advanceMs(40); // t=100 > 90
    ec.runDue(clock.now(), &rec);
    const Rec::Ev& last = rec.events.last();
    CHECK(last.action == KTouch::Up);
    CHECK(last.pointer == 1002);
}

static void testOneKeyMultipleNodes()
{
    auto p = makeProfile();
    p.nodes.append(buttonNode(QStringLiteral("Q"), 0.1, 0.1, mw::keymap::ButtonMode::Tap));
    p.nodes.append(buttonNode(QStringLiteral("Q"), 0.2, 0.2, mw::keymap::ButtonMode::Tap));
    FakeClock clock;
    EngineCore ec(std::move(p), { kW, kH }, clock.fn());
    Rec rec;
    keyPress(ec, rec, QStringLiteral("Q"), true);
    CHECK(rec.events.size() == 4); // 两处各 Down+Up
}

// ---- nextDeadline：假时钟下的最早到期 ----

static void testNextDeadline()
{
    auto p = makeProfile();
    p.nodes.append(buttonNode(QStringLiteral("L"), 0.3, 0.3, mw::keymap::ButtonMode::Repeat, 100));
    FakeClock clock;
    EngineCore ec(std::move(p), { kW, kH }, clock.fn());
    Rec rec;
    keyPress(ec, rec, QStringLiteral("L"), true);
    const auto dl = ec.nextDeadline();
    CHECK(dl.has_value());
    CHECK(std::chrono::duration_cast<std::chrono::milliseconds>(*dl - clock.now()).count() == 100);
}

// ---- AndroidKeycode 往返 ----

static void testAndroidKeycode()
{
    // 字符
    for (char32_t c = U'a'; c <= U'z'; ++c) {
        const AndroidKeycode k = AndroidKeycode::fromChar(c);
        CHECK(k.k == AndroidKeycode::K::Char);
        CHECK(AndroidKeycode::fromI32(k.toI32()).ch == c);
    }
    for (char32_t c = U'0'; c <= U'9'; ++c) {
        const AndroidKeycode k = AndroidKeycode::fromChar(c);
        CHECK(k.k == AndroidKeycode::K::Char);
        CHECK(AndroidKeycode::fromI32(k.toI32()).ch == c);
    }
    // 大写归一小写
    CHECK(AndroidKeycode::fromChar(U'V').toI32() == AndroidKeycode::fromChar(U'v').toI32());
    // 符号键
    CHECK(AndroidKeycode::fromChar(U' ').k == AndroidKeycode::K::Space);
    CHECK(AndroidKeycode::fromChar(U'\n').k == AndroidKeycode::K::Enter);
    CHECK(AndroidKeycode::fromChar(U'[').toI32() == 71);
    // 功能键数值（AOSP）
    CHECK(AndroidKeycode::fromI32(3).k == AndroidKeycode::K::Home);
    CHECK(AndroidKeycode::fromI32(4).k == AndroidKeycode::K::Back);
    CHECK(AndroidKeycode::fromI32(187).k == AndroidKeycode::K::AppSwitch);
    CHECK(AndroidKeycode::fromI32(131).k == AndroidKeycode::K::F1);
    CHECK(AndroidKeycode::fromI32(142).k == AndroidKeycode::K::F12);
    CHECK(AndroidKeycode::fromI32(66).k == AndroidKeycode::K::Enter);
    // 未知
    CHECK(AndroidKeycode::fromChar(U'$').k == AndroidKeycode::K::Unknown);
    CHECK(AndroidKeycode::fromI32(0).k == AndroidKeycode::K::Unknown);
}

// ---- InputRouter + EngineSink（假注入器）----

namespace {

struct Injected {
    QString op;
    quint32 display = 0;
    TouchAction action = TouchAction::Down;
    quint64 pointer = 0;
    qint32 x = 0, y = 0;
    float h = 0.0f, v = 0.0f;
};

class MockInjector : public InputInjector {
public:
    QVector<Injected> calls;
    quint32 acceptDisplay = 0;

    mw::Result<bool> injectTouch(quint32 displayId, TouchAction action, quint64 pointerId,
                                 qint32 x, qint32 y) override
    {
        Injected c;
        c.op = QStringLiteral("touch");
        c.display = displayId;
        c.action = action;
        c.pointer = pointerId;
        c.x = x;
        c.y = y;
        calls.append(c);
        return mw::Result<bool>::Ok();
    }
    mw::Result<bool> injectKey(const AndroidKeycode&, KeyAction) override
    {
        calls.append({ QStringLiteral("key") });
        return mw::Result<bool>::Ok();
    }
    mw::Result<bool> injectText(const QString& text) override
    {
        Injected c;
        c.op = QStringLiteral("text");
        c.x = text.size();
        calls.append(c);
        return mw::Result<bool>::Ok();
    }
    mw::Result<bool> injectScroll(quint32 displayId, qint32 x, qint32 y, float h,
                                  float v) override
    {
        Injected c;
        c.op = QStringLiteral("scroll");
        c.display = displayId;
        c.x = x;
        c.y = y;
        c.h = h;
        c.v = v;
        calls.append(c);
        return mw::Result<bool>::Ok();
    }
};

} // namespace

static void testRouterAndEngineSink()
{
    auto def = std::make_unique<MockInjector>();
    MockInjector* defRaw = def.get();
    InputRouter router(std::move(def));

    auto bound = std::make_unique<MockInjector>();
    MockInjector* boundRaw = bound.get();
    router.bindDisplay(2, std::move(bound));

    // 未绑定的 displayId 落 default；绑定的落 bound
    router.injectTouch(0, TouchAction::Down, 1, 10, 20);
    CHECK(defRaw->calls.size() == 1 && defRaw->calls[0].display == 0);
    router.injectTouch(2, TouchAction::Move, 1, 30, 40);
    CHECK(boundRaw->calls.size() == 1 && boundRaw->calls[0].display == 2);
    CHECK(router.boundDisplays() == QVector<quint32>{ 2 });
    // key/text 恒走 default
    router.injectKey(AndroidKeycode::fromChar(U'a'), KeyAction::Down);
    CHECK(defRaw->calls.last().op == QStringLiteral("key"));
    router.injectText(QStringLiteral("hi"));
    CHECK(defRaw->calls.last().op == QStringLiteral("text"));
    // 解绑后再注入回落 default
    CHECK(router.unbindDisplay(2));
    router.injectScroll(2, 1, 2, 0.0f, 1.0f);
    CHECK(defRaw->calls.last().op == QStringLiteral("scroll"));
    CHECK(defRaw->calls.last().display == 2);

    // EngineSink：keymap TouchAction → input TouchAction，带 displayId 转发
    auto shared = std::make_shared<SharedInjector>(std::make_unique<MockInjector>());
    MockInjector* sinkRaw = static_cast<MockInjector*>(shared->injector.get());
    EngineSink sink(shared, 5);
    sink.touch(mw::keymap::TouchAction::Down, 1001, 111, 222);
    CHECK(sinkRaw->calls.size() == 1);
    CHECK(sinkRaw->calls[0].op == QStringLiteral("touch"));
    CHECK(sinkRaw->calls[0].display == 5);
    CHECK(sinkRaw->calls[0].action == TouchAction::Down);
    CHECK(sinkRaw->calls[0].pointer == 1001);
    CHECK(sinkRaw->calls[0].x == 111 && sinkRaw->calls[0].y == 222);
}

// ---- adb 纯函数 ----

static void testAdbPureFunctions()
{
    // shellQuote：单引号转义
    CHECK(shellQuote(QStringLiteral("abc")) == QStringLiteral("'abc'"));
    CHECK(shellQuote(QStringLiteral("a'b")) == QStringLiteral("'a'\\''b'"));
    // textArg：空格 → %s 再引号
    CHECK(textArg(QStringLiteral("hello world")) == QStringLiteral("'hello%sworld'"));
    // displayPrefix：主屏省略
    CHECK(displayPrefix(0).isEmpty());
    CHECK(displayPrefix(2) == QStringLiteral("--display 2 "));
    // scrollTarget：正 v（滚轮向上）= 手指向下滑（y 增大）
    const QPair<qint32, qint32> down = scrollTarget(500, 500, 0.0f, 1.0f);
    CHECK(down.first == 500 && down.second == 500 + 240);
    const QPair<qint32, qint32> left = scrollTarget(500, 500, -1.0f, 0.0f);
    CHECK(left.first == 500 - 240 && left.second == 500);
    // 超界钳制：2.0 与 1.0 等效
    const QPair<qint32, qint32> over = scrollTarget(0, 0, 0.0f, 2.0f);
    const QPair<qint32, qint32> norm = scrollTarget(0, 0, 0.0f, 1.0f);
    CHECK(over == norm);
    // planTouchUp：按住不动 → Tap；位移 → Swipe（时长下限 100ms）
    {
        const auto t0 = std::chrono::steady_clock::now();
        PendingTouch still{ { 10, 20 }, { 10, 20 }, t0 };
        const TouchPlan tap = planTouchUp(still, t0 + std::chrono::milliseconds(700));
        CHECK(tap.kind == TouchPlan::Kind::Tap);
        CHECK(tap.x == 10 && tap.y == 20);
        PendingTouch moved{ { 10, 20 }, { 110, 220 }, t0 };
        const TouchPlan swipe = planTouchUp(moved, t0 + std::chrono::milliseconds(700));
        CHECK(swipe.kind == TouchPlan::Kind::Swipe);
        CHECK(swipe.x1 == 10 && swipe.y1 == 20 && swipe.x2 == 110 && swipe.y2 == 220);
        CHECK(swipe.durationMs == 700);
        PendingTouch quick{ { 10, 20 }, { 110, 220 }, t0 };
        const TouchPlan minSwipe = planTouchUp(quick, t0 + std::chrono::milliseconds(30));
        CHECK(minSwipe.durationMs == 100); // 下限
    }
}

// ---- console 纯函数 ----

static void testConsolePureFunctions()
{
    CHECK(consolePortOfSerial(QStringLiteral("emulator-5554")) == 5554);
    CHECK(consolePortOfSerial(QStringLiteral("emulator-abc")) == 0);
    CHECK(consolePortOfSerial(QStringLiteral("usb-123")) == 0);

    // touchCommand：坐标域换算 0..32767
    const QString down = touchCommand({ 1000, 500 }, TouchAction::Down, 0, 3, 500, 250);
    CHECK(down.contains(QStringLiteral("ABS_MT_SLOT:0")));
    CHECK(down.contains(QStringLiteral("ABS_MT_TRACKING_ID:3")));
    CHECK(down.contains(QStringLiteral("ABS_MT_POSITION_X:16384"))); // round(0.5*32767)
    CHECK(down.contains(QStringLiteral("ABS_MT_POSITION_Y:16384")));
    CHECK(down.endsWith(QStringLiteral("EV_SYN:0:0")));
    const QString up = touchCommand({ 1000, 500 }, TouchAction::Up, 2, -1, 0, 0);
    CHECK(up.contains(QStringLiteral("ABS_MT_SLOT:2")));
    CHECK(up.contains(QStringLiteral("ABS_MT_TRACKING_ID:-1")));

    // Fingers：slot 复用 + tracking 递增 + 上限
    Fingers f;
    const auto a = f.alloc(1001);
    const auto b = f.alloc(1002);
    CHECK(a.has_value() && a->first == 0 && a->second == 0);
    CHECK(b.has_value() && b->first == 1 && b->second == 1);
    f.map.remove(1001); // 抬起 slot 0
    const auto c = f.alloc(1003);
    CHECK(c.has_value() && c->first == 0 && c->second == 2); // 复用最小空闲 slot
    for (quint64 pid = 2000; pid < 2120; ++pid)
        f.alloc(pid);
    CHECK(!f.alloc(9999).has_value()); // 超 11 指上限
}

// ---- scrcpy 注入消息构造（经 encode 做字节断言）----

static void testScrcpyMessages()
{
    // touchMessage：Down 主键按下、pressure=1
    {
        const auto m = touchMessage({ 1280, 720 }, TouchAction::Down, 0xFFFFFFFFFFFFFFFFULL, 640,
                                     360);
        auto b = m.encode();
        CHECK(b.ok());
        CHECK(quint8(b.value()[0]) == 2);
        CHECK(quint8(b.value()[1]) == 0); // ACTION_DOWN
        // pressure u16 定点在 offset 2+8+4+4+2+2 = 22..24 → 0xffff
        CHECK(quint8(b.value()[22]) == 0xFF && quint8(b.value()[23]) == 0xFF);
        // Up：pressure 0、buttons 0
        const auto up = touchMessage({ 1280, 720 }, TouchAction::Up, 7, 1, 2).encode();
        CHECK(up.ok());
        CHECK(quint8(up.value()[22]) == 0x00 && quint8(up.value()[23]) == 0x00);
    }
    // scrollMessage / keyMessage
    {
        const auto sm = scrollMessage({ 1000, 500 }, 10, 20, 0.0f, -1.0f).encode();
        CHECK(sm.ok());
        CHECK(quint8(sm.value()[0]) == 3);
        const auto km = keyMessage(AndroidKeycode::fromChar(U'c'), KeyAction::Down).encode();
        CHECK(km.ok());
        CHECK(quint8(km.value()[0]) == 0);
        CHECK(quint8(km.value()[1]) == 0);
    }
    // splitText：300 字节分块 + UTF-8 边界
    {
        CHECK(splitText(QString()).size() == 1); // 空 → 单空块（保持 Ok 契约）
        CHECK(splitText(QStringLiteral("ok")).size() == 1);
        QString big;
        for (int i = 0; i < 100; ++i)
            big += QStringLiteral("你好世界"); // 每字 3 字节 → 12 字节/组
        const auto chunks = splitText(big); // 1200 字节 → ≥4 块
        CHECK(chunks.size() >= 4);
        int total = 0;
        for (const QByteArray& c : chunks) {
            CHECK(c.size() <= 300);
            CHECK(c.size() % 3 == 0); // 未撕裂 UTF-8（全 3 字节字符场景）
            total += c.size();
        }
        CHECK(total == big.toUtf8().size());
    }
}

// ---- event_tap 纯函数：keyName / modifierOf / classify ----

static void testClassify()
{
    // 字母键
    {
        RawEvent raw;
        raw.eventType = kEvtKeyDown;
        raw.keycode = 0x00; // A
        raw.flags = 0;
        auto ev = classify(raw);
        CHECK(ev.has_value());
        CHECK(ev->kind == HostInputEvent::Kind::Key);
        CHECK(ev->key == QStringLiteral("A"));
        CHECK(ev->action == HostKeyAction::Down);
        CHECK(!ev->autorepeat);
        raw.eventType = kEvtKeyUp;
        raw.autorepeat = true;
        ev = classify(raw);
        CHECK(ev.has_value() && ev->action == HostKeyAction::Up);
        CHECK(!ev->autorepeat); // Up 不带 autorepeat
    }
    // 未收录键码 → nullopt
    {
        RawEvent raw;
        raw.eventType = kEvtKeyDown;
        raw.keycode = 0x0A; // 未定义
        CHECK(!classify(raw).has_value());
    }
    // 修饰键 FlagsChanged：按 flags 位判 Down/Up
    {
        RawEvent raw;
        raw.eventType = kEvtFlagsChanged;
        raw.keycode = 0x38; // ShiftLeft
        raw.flags = kMaskShift;
        auto ev = classify(raw);
        CHECK(ev.has_value() && ev->key == QStringLiteral("ShiftLeft")
              && ev->action == HostKeyAction::Down);
        raw.flags = 0;
        ev = classify(raw);
        CHECK(ev.has_value() && ev->action == HostKeyAction::Up);
        // 非修饰键的 FlagsChanged → nullopt
        raw.keycode = 0x00;
        CHECK(!classify(raw).has_value());
    }
    // 鼠标按键
    {
        RawEvent raw;
        raw.eventType = kEvtRightMouseDown;
        raw.flags = 0x100;
        auto ev = classify(raw);
        CHECK(ev.has_value() && ev->kind == HostInputEvent::Kind::MouseButton);
        CHECK(ev->button.keyName() == QStringLiteral("MouseRight"));
        CHECK(ev->action == HostKeyAction::Down);
        CHECK(ev->mods == 0x100);
    }
    // 中键走 Other 通道（buttonNumber=2）
    {
        RawEvent raw;
        raw.eventType = kEvtOtherMouseDown;
        raw.buttonNumber = 2;
        auto ev = classify(raw);
        CHECK(ev.has_value() && ev->button.keyName() == QStringLiteral("MouseMiddle"));
        raw.buttonNumber = 3;
        ev = classify(raw);
        CHECK(ev.has_value() && ev->button.keyName() == QStringLiteral("Mouse3"));
    }
    // 移动 / 滚轮
    {
        RawEvent raw;
        raw.eventType = kEvtMouseMoved;
        raw.dx = 3.5;
        raw.dy = -1.5;
        auto ev = classify(raw);
        CHECK(ev.has_value() && ev->kind == HostInputEvent::Kind::MouseMoved);
        CHECK(near(ev->dx, 3.5) && near(ev->dy, -1.5));
        raw.eventType = kEvtScrollWheel;
        raw.scrollDx = 0;
        raw.scrollDy = -12;
        ev = classify(raw);
        CHECK(ev.has_value() && ev->kind == HostInputEvent::Kind::Scroll);
        CHECK(near(ev->scrollDy, -12.0));
    }
    // 无关事件类型 → nullopt
    {
        RawEvent raw;
        raw.eventType = 63; // 系统定义外
        CHECK(!classify(raw).has_value());
    }
    // modifierOf
    {
        QString name;
        quint64 mask = 0;
        CHECK(modifierOf(0x37, &name, &mask) && name == QStringLiteral("CommandLeft")
              && mask == kMaskCommand);
        CHECK(!modifierOf(0x00, &name, &mask));
    }
    // keyName 命名抽查（与 keymap 档案一致）
    CHECK(keyName(0x31) == QStringLiteral("Space"));
    CHECK(keyName(0x35) == QStringLiteral("Escape"));
    CHECK(keyName(0x7E) == QStringLiteral("Up"));
    CHECK(keyName(0x24) == QStringLiteral("Enter"));
    CHECK(keyName(0x33) == QStringLiteral("Backspace"));
}

// ---- KeymapRunner：焦点门控旁路下的吞键策略 ----

static void testKeymapRunnerPolicy()
{
    auto p = makeProfile();
    p.nodes.append(buttonNode(QStringLiteral("J"), 0.1, 0.1, mw::keymap::ButtonMode::Tap));
    auto sink = std::make_shared<Rec>();
    auto engine = Engine::create(std::move(p), { kW, kH },
                                 SharedSink{ sink, std::make_shared<std::mutex>() });
    KeymapRunner runner(engine);
    runner.setFocusGateOff(true); // 单测无 qemu 窗口

    // 未绑定键：放行
    HostInputEvent unbound = HostInputEvent::keyEvent(QStringLiteral("Z"), HostKeyAction::Down,
                                                      false, 0);
    CHECK(!runner.handleEvent(unbound));
    // 绑定键：吞掉
    HostInputEvent bound = HostInputEvent::keyEvent(QStringLiteral("J"), HostKeyAction::Down,
                                                    false, 0);
    CHECK(runner.handleEvent(bound));
    CHECK(sink->events.size() == 2); // tap = Down+Up
    // 自动重复：吞掉但不重复注入
    HostInputEvent repeat = HostInputEvent::keyEvent(QStringLiteral("J"), HostKeyAction::Down,
                                                     true, 0);
    CHECK(runner.handleEvent(repeat));
    CHECK(sink->events.size() == 2);
    // Cmd 组合键：放行
    HostInputEvent combo = HostInputEvent::keyEvent(QStringLiteral("J"), HostKeyAction::Down,
                                                    false, kMaskCommand);
    CHECK(!runner.handleEvent(combo));
    // 总开关：吞掉
    HostInputEvent sw = HostInputEvent::keyEvent(QStringLiteral("Grave"), HostKeyAction::Down,
                                                 false, 0);
    CHECK(runner.handleEvent(sw));
    CHECK(!engine->active());
    runner.shutdown();
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    testJoystickTarget();
    testJoystickFullCycle();
    testButtonModes();
    testSkillAimCastAndCancel();
    testLookCaptureAndWraparound();
    testSwitchKeyTogglesAndReleases();
    testSwipeInterpolates();
    testWheelScrollTaps();
    testMacroSequence();
    testOneKeyMultipleNodes();
    testNextDeadline();
    testAndroidKeycode();
    testRouterAndEngineSink();
    testAdbPureFunctions();
    testConsolePureFunctions();
    testScrcpyMessages();
    testClassify();
    testKeymapRunnerPolicy();

    if (g_failures == 0) {
        qInfo().noquote() << QStringLiteral("mw_input_test: 全部通过");
        return 0;
    }
    qCritical().noquote() << QStringLiteral("mw_input_test: %1 项失败").arg(g_failures);
    return 1;
}
