// 键位档案 v2 数据模型：parse / toJson / sanitize / v1 迁移
//（移植自 crates/mwine-keymap/src/lib.rs，序列化与 serde 字段名一一对应）。
#include "keymap.hpp"

#include <QJsonArray>
#include <QJsonDocument>
#include <QStringList>

#include <algorithm>
#include <cmath>

#include "json.hpp"

namespace mw::keymap {
namespace {

// ---- 便捷取值（缺字段/类型不符 → serde default）----
QString strOr(const QJsonObject& o, const char* k, const QString& d)
{
    const auto v = o.value(QLatin1String(k));
    return v.isString() ? v.toString() : d;
}

double numOr(const QJsonObject& o, const char* k, double d)
{
    return mw::jNum(o, k, d);
}

// 必填字段（serde 无 default 的字段）：返回空串表示成功，否则返回错误明细
QString reqNum(const QJsonObject& o, const char* k, double* out)
{
    const auto v = o.value(QLatin1String(k));
    if (!v.isDouble())
        return QStringLiteral("缺少必填数字字段 %1").arg(QString::fromLatin1(k));
    *out = v.toDouble();
    return QString();
}

QString reqStr(const QJsonObject& o, const char* k, QString* out)
{
    const auto v = o.value(QLatin1String(k));
    if (!v.isString())
        return QStringLiteral("缺少必填字符串字段 %1").arg(QString::fromLatin1(k));
    *out = v.toString();
    return QString();
}

// ---- 枚举名（serde rename_all = "lowercase"）----
QString mouseModeName(MouseMode m)
{
    return m == MouseMode::Free ? QStringLiteral("free") : QStringLiteral("capture");
}
QString buttonModeName(ButtonMode m)
{
    switch (m) {
    case ButtonMode::Tap: return QStringLiteral("tap");
    case ButtonMode::Hold: return QStringLiteral("hold");
    case ButtonMode::Repeat: return QStringLiteral("repeat");
    case ButtonMode::Toggle: return QStringLiteral("toggle");
    }
    return QStringLiteral("tap");
}
QString skillModeName(SkillMode m)
{
    return m == SkillMode::Quick ? QStringLiteral("quick") : QStringLiteral("aim");
}

bool parseMouseMode(const QString& s, MouseMode* out)
{
    if (s == QLatin1String("free")) { *out = MouseMode::Free; return true; }
    if (s == QLatin1String("capture")) { *out = MouseMode::Capture; return true; }
    return false;
}

bool parseButtonMode(const QString& s, ButtonMode* out)
{
    if (s == QLatin1String("tap")) { *out = ButtonMode::Tap; return true; }
    if (s == QLatin1String("hold")) { *out = ButtonMode::Hold; return true; }
    if (s == QLatin1String("repeat")) { *out = ButtonMode::Repeat; return true; }
    if (s == QLatin1String("toggle")) { *out = ButtonMode::Toggle; return true; }
    return false;
}

bool parseSkillMode(const QString& s, SkillMode* out)
{
    if (s == QLatin1String("quick")) { *out = SkillMode::Quick; return true; }
    if (s == QLatin1String("aim")) { *out = SkillMode::Aim; return true; }
    return false;
}

bool lookFromJson(const QJsonObject& o, LookConfig* out, QString* err)
{
    LookConfig l;
    QString e = reqNum(o, "x", &l.x);
    if (e.isEmpty())
        e = reqNum(o, "y", &l.y);
    if (!e.isEmpty()) {
        *err = QStringLiteral("键位档案格式错误: look: %1").arg(e);
        return false;
    }
    l.sensX = numOr(o, "sensX", 3.2);
    l.sensY = numOr(o, "sensY", 2.0);
    const auto bv = o.value(QLatin1String("bounds"));
    if (!bv.isUndefined() && !bv.isNull()) {
        if (!bv.isArray()) {
            *err = QStringLiteral("键位档案格式错误: look.bounds 应为 4 个数字的数组");
            return false;
        }
        const QJsonArray a = bv.toArray();
        bool ok = a.size() == 4;
        for (int i = 0; ok && i < 4; ++i)
            ok = a.at(i).isDouble();
        if (!ok) {
            *err = QStringLiteral("键位档案格式错误: look.bounds 应为 4 个数字的数组");
            return false;
        }
        for (int i = 0; i < 4; ++i)
            l.bounds[size_t(i)] = a.at(i).toDouble();
        l.hasBounds = true;
    }
    *out = l;
    return true;
}

bool macroStepFromJson(const QJsonObject& o, MacroStep* out, QString* err)
{
    MacroStep m;
    const QString action = strOr(o, "action", QString());
    QString e;
    if (action == QLatin1String("tap") || action == QLatin1String("touchDown")) {
        m.action = action == QLatin1String("tap") ? MacroStep::Action::Tap
                                                  : MacroStep::Action::TouchDown;
        if (!(e = reqNum(o, "x", &m.x)).isEmpty() || !(e = reqNum(o, "y", &m.y)).isEmpty()) {
            *err = QStringLiteral("宏步骤 %1: %2").arg(action, e);
            return false;
        }
        m.delayMs = quint32(numOr(o, "delayMs", 0));
    } else if (action == QLatin1String("touchUp")) {
        m.action = MacroStep::Action::TouchUp;
        m.delayMs = quint32(numOr(o, "delayMs", 0));
    } else if (action == QLatin1String("swipe")) {
        m.action = MacroStep::Action::Swipe;
        if (!(e = reqNum(o, "x", &m.x)).isEmpty() || !(e = reqNum(o, "y", &m.y)).isEmpty()
            || !(e = reqNum(o, "x2", &m.x2)).isEmpty()
            || !(e = reqNum(o, "y2", &m.y2)).isEmpty()) {
            *err = QStringLiteral("宏步骤 swipe: %1").arg(e);
            return false;
        }
        m.durationMs = quint32(numOr(o, "durationMs", 200));
        m.delayMs = quint32(numOr(o, "delayMs", 0));
    } else if (action == QLatin1String("delay")) {
        m.action = MacroStep::Action::Delay;
        double ms = 0.0;
        if (!(e = reqNum(o, "ms", &ms)).isEmpty()) {
            *err = QStringLiteral("宏步骤 delay: %1").arg(e);
            return false;
        }
        m.ms = quint32(ms);
    } else {
        *err = QStringLiteral("未知宏步骤类型: %1").arg(action.isEmpty() ? QStringLiteral("(缺失)") : action);
        return false;
    }
    *out = m;
    return true;
}

bool nodeFromJson(const QJsonObject& o, Node* out, QString* err)
{
    Node n; // 默认成员值 = serde default
    n.comment = strOr(o, "comment", QString());
    const QString type = strOr(o, "type", QString());
    QString e;
    if (type == QLatin1String("joystick")) {
        n.kind = Node::Kind::Joystick;
        if (!(e = reqNum(o, "x", &n.x)).isEmpty() || !(e = reqNum(o, "y", &n.y)).isEmpty()) {
            *err = e;
            return false;
        }
        n.radius = numOr(o, "radius", 0.16);
        n.up = strOr(o, "up", QStringLiteral("W"));
        n.down = strOr(o, "down", QStringLiteral("S"));
        n.left = strOr(o, "left", QStringLiteral("A"));
        n.right = strOr(o, "right", QStringLiteral("D"));
    } else if (type == QLatin1String("button")) {
        n.kind = Node::Kind::Button;
        if (!(e = reqStr(o, "key", &n.key)).isEmpty()
            || !(e = reqNum(o, "x", &n.x)).isEmpty()
            || !(e = reqNum(o, "y", &n.y)).isEmpty()) {
            *err = e;
            return false;
        }
        if (!parseButtonMode(strOr(o, "mode", QStringLiteral("tap")), &n.buttonMode)) {
            *err = QStringLiteral("未知按键模式: %1").arg(strOr(o, "mode", QString()));
            return false;
        }
        n.intervalMs = quint32(numOr(o, "intervalMs", 120));
    } else if (type == QLatin1String("skill")) {
        n.kind = Node::Kind::Skill;
        if (!(e = reqStr(o, "key", &n.key)).isEmpty()
            || !(e = reqNum(o, "x", &n.x)).isEmpty()
            || !(e = reqNum(o, "y", &n.y)).isEmpty()) {
            *err = e;
            return false;
        }
        if (!parseSkillMode(strOr(o, "mode", QStringLiteral("quick")), &n.skillMode)) {
            *err = QStringLiteral("未知技能模式: %1").arg(strOr(o, "mode", QString()));
            return false;
        }
        n.dragRadius = numOr(o, "dragRadius", 0.18);
        n.cancelable = mw::jBool(o, "cancelable", true);
    } else if (type == QLatin1String("swipe")) {
        n.kind = Node::Kind::Swipe;
        if (!(e = reqStr(o, "key", &n.key)).isEmpty()
            || !(e = reqNum(o, "x", &n.x)).isEmpty()
            || !(e = reqNum(o, "y", &n.y)).isEmpty()
            || !(e = reqNum(o, "x2", &n.x2)).isEmpty()
            || !(e = reqNum(o, "y2", &n.y2)).isEmpty()) {
            *err = e;
            return false;
        }
        n.durationMs = quint32(numOr(o, "durationMs", 200));
    } else if (type == QLatin1String("wheel")) {
        n.kind = Node::Kind::Wheel;
        if (!(e = reqNum(o, "upX", &n.upX)).isEmpty()
            || !(e = reqNum(o, "upY", &n.upY)).isEmpty()
            || !(e = reqNum(o, "downX", &n.downX)).isEmpty()
            || !(e = reqNum(o, "downY", &n.downY)).isEmpty()) {
            *err = e;
            return false;
        }
    } else if (type == QLatin1String("macro")) {
        n.kind = Node::Kind::Macro;
        if (!(e = reqStr(o, "key", &n.key)).isEmpty()) {
            *err = e;
            return false;
        }
        const auto sv = o.value(QLatin1String("steps"));
        if (!sv.isUndefined() && !sv.isNull()) {
            if (!sv.isArray()) {
                *err = QStringLiteral("steps 应为数组");
                return false;
            }
            int j = 0;
            for (const auto& ev : sv.toArray()) {
                if (!ev.isObject()) {
                    *err = QStringLiteral("steps[%1] 应为对象").arg(j);
                    return false;
                }
                MacroStep ms;
                if (!macroStepFromJson(ev.toObject(), &ms, err)) {
                    *err = QStringLiteral("steps[%1]: %2").arg(j).arg(*err);
                    return false;
                }
                n.steps.append(ms);
                ++j;
            }
        }
    } else {
        *err = QStringLiteral("未知节点类型: %1").arg(type.isEmpty() ? QStringLiteral("(缺失)") : type);
        return false;
    }
    *out = n;
    return true;
}

bool profileFromJson(const QJsonObject& o, Profile* out, QString* err)
{
    Profile p; // 全默认；缺字段取默认，未知字段忽略
    p.version = quint32(numOr(o, "version", double(kSchemaVersion)));
    p.name = strOr(o, "name", QString());
    p.package = strOr(o, "package", QString());
    p.switchKey = strOr(o, "switchKey", QStringLiteral("Grave"));
    if (!parseMouseMode(strOr(o, "mouseMode", QStringLiteral("free")), &p.mouseMode)) {
        *err = QStringLiteral("键位档案格式错误: 未知鼠标模式: %1").arg(strOr(o, "mouseMode", QString()));
        return false;
    }
    const auto lv = o.value(QLatin1String("look"));
    if (lv.isObject()) {
        if (!lookFromJson(lv.toObject(), &p.look, err))
            return false;
        p.hasLook = true;
    } else if (!lv.isUndefined() && !lv.isNull()) {
        *err = QStringLiteral("键位档案格式错误: look 应为对象");
        return false;
    }
    const auto av = o.value(QLatin1String("activity"));
    if (av.isString()) {
        p.activity = av.toString();
        p.hasActivity = true;
    } else if (!av.isUndefined() && !av.isNull()) {
        *err = QStringLiteral("键位档案格式错误: activity 应为字符串");
        return false;
    }
    const auto nv = o.value(QLatin1String("nodes"));
    if (!nv.isUndefined() && !nv.isNull()) {
        if (!nv.isArray()) {
            *err = QStringLiteral("键位档案格式错误: nodes 应为数组");
            return false;
        }
        int i = 0;
        for (const auto& ev : nv.toArray()) {
            if (!ev.isObject()) {
                *err = QStringLiteral("键位档案格式错误: nodes[%1] 应为对象").arg(i);
                return false;
            }
            Node n;
            QString detail;
            if (!nodeFromJson(ev.toObject(), &n, &detail)) {
                *err = QStringLiteral("键位档案格式错误: nodes[%1]: %2").arg(i).arg(detail);
                return false;
            }
            p.nodes.append(n);
            ++i;
        }
    }
    *out = p;
    return true;
}

// v1 小写键名 → v2 命名（与 event_tap::key_name 一致）。
QString mapV1Key(const QString& k)
{
    struct Rule { const char* from; const char* to; };
    static const Rule rules[] = {
        { "space", "Space" },        { "return", "Enter" },     { "enter", "Enter" },
        { "tab", "Tab" },            { "delete", "Backspace" }, { "backspace", "Backspace" },
        { "escape", "Escape" },      { "up", "Up" },            { "down", "Down" },
        { "left", "Left" },          { "right", "Right" },      { "grave", "Grave" },
        { "shift", "ShiftLeft" },    { "ctrl", "ControlLeft" }, { "control", "ControlLeft" },
        { "alt", "OptionLeft" },     { "option", "OptionLeft" }, { "cmd", "CommandLeft" },
        { "command", "CommandLeft" },
    };
    for (const auto& r : rules)
        if (k == QLatin1String(r.from))
            return QString::fromLatin1(r.to);
    if (k.size() == 1)
        return k.toUpper(); // 单字母键大写
    return k;
}

// v1 扁平绑定数组 → v2。字段：key/type(tap|hold|swipe)/x/y/x2/y2/duration/interval，
// 坐标为 Android 像素，键名小写。
mw::Result<Profile> migrateV1(const QJsonArray& arr, QPair<int, int> screen)
{
    Profile p;
    const double w = double(screen.first);
    const double h = double(screen.second);
    auto normX = [w](double x) { return std::clamp(x / w, 0.0, 1.0); };
    auto normY = [h](double y) { return std::clamp(y / h, 0.0, 1.0); };
    int i = 0;
    for (const auto& ev : arr) {
        if (!ev.isObject())
            return mw::err<Profile>(QStringLiteral("v1 绑定项格式错误: 项 %1 不是对象").arg(i));
        const QJsonObject b = ev.toObject();
        const QString key = mapV1Key(strOr(b, "key", QString()));
        const QString type = strOr(b, "type", QStringLiteral("tap"));
        const double x = numOr(b, "x", 0.0);
        const double y = numOr(b, "y", 0.0);
        const auto x2v = b.value(QLatin1String("x2"));
        const auto y2v = b.value(QLatin1String("y2"));
        const auto durv = b.value(QLatin1String("duration"));
        const auto intv = b.value(QLatin1String("interval"));
        if (type == QLatin1String("swipe")) {
            Node n;
            n.kind = Node::Kind::Swipe;
            n.comment = QString();
            n.key = key;
            n.x = normX(x);
            n.y = normY(y);
            n.x2 = normX(x2v.isDouble() ? x2v.toDouble() : x + 300.0);
            n.y2 = normY(y2v.isDouble() ? y2v.toDouble() : y);
            n.durationMs = std::max<quint32>(quint32(durv.isDouble() ? durv.toDouble() : 300.0), 30);
            p.nodes.append(n);
        } else if (type == QLatin1String("hold") || type == QLatin1String("repeat")) {
            // v1 的 "hold" 实为按住连点（MWineKeys 用 Timer 重复 tap）
            Node n;
            n.kind = Node::Kind::Button;
            n.comment = QString();
            n.key = key;
            n.x = normX(x);
            n.y = normY(y);
            n.buttonMode = ButtonMode::Repeat;
            n.intervalMs = std::max<quint32>(quint32(intv.isDouble() ? intv.toDouble() : 120.0), 40);
            p.nodes.append(n);
        } else {
            Node n;
            n.kind = Node::Kind::Button;
            n.comment = QString();
            n.key = key;
            n.x = normX(x);
            n.y = normY(y);
            n.buttonMode = ButtonMode::Tap;
            n.intervalMs = 120; // default_interval()，tap 分支不做 max
            p.nodes.append(n);
        }
        ++i;
    }
    return mw::Result<Profile>::Ok(p);
}

} // namespace

// ===== LookConfig =====

std::array<double, 4> LookConfig::boundsArr() const
{
    if (hasBounds)
        return bounds;
    return { 0.3, 0.05, 0.98, 0.95 };
}

QJsonObject LookConfig::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("x"), x);
    o.insert(QStringLiteral("y"), y);
    o.insert(QStringLiteral("sensX"), sensX);
    o.insert(QStringLiteral("sensY"), sensY);
    if (hasBounds) { // None 不序列化（serde skip_serializing_if）
        QJsonArray a;
        for (const double v : bounds)
            a.append(v);
        o.insert(QStringLiteral("bounds"), a);
    }
    return o;
}

// ===== MacroStep =====

QJsonObject MacroStep::toJson() const
{
    QJsonObject o;
    switch (action) {
    case Action::Tap:
        o.insert(QStringLiteral("action"), QStringLiteral("tap"));
        o.insert(QStringLiteral("x"), x);
        o.insert(QStringLiteral("y"), y);
        o.insert(QStringLiteral("delayMs"), double(delayMs));
        break;
    case Action::TouchDown:
        o.insert(QStringLiteral("action"), QStringLiteral("touchDown"));
        o.insert(QStringLiteral("x"), x);
        o.insert(QStringLiteral("y"), y);
        o.insert(QStringLiteral("delayMs"), double(delayMs));
        break;
    case Action::TouchUp:
        o.insert(QStringLiteral("action"), QStringLiteral("touchUp"));
        o.insert(QStringLiteral("delayMs"), double(delayMs));
        break;
    case Action::Swipe:
        o.insert(QStringLiteral("action"), QStringLiteral("swipe"));
        o.insert(QStringLiteral("x"), x);
        o.insert(QStringLiteral("y"), y);
        o.insert(QStringLiteral("x2"), x2);
        o.insert(QStringLiteral("y2"), y2);
        o.insert(QStringLiteral("durationMs"), double(durationMs));
        o.insert(QStringLiteral("delayMs"), double(delayMs));
        break;
    case Action::Delay:
        o.insert(QStringLiteral("action"), QStringLiteral("delay"));
        o.insert(QStringLiteral("ms"), double(ms));
        break;
    }
    return o;
}

// ===== Node =====

QString Node::kindName() const
{
    switch (kind) {
    case Kind::Joystick: return QStringLiteral("摇杆");
    case Kind::Button: return QStringLiteral("按键");
    case Kind::Skill: return QStringLiteral("技能");
    case Kind::Swipe: return QStringLiteral("滑动");
    case Kind::Wheel: return QStringLiteral("滚轮");
    case Kind::Macro: return QStringLiteral("宏");
    }
    return QString();
}

bool Node::hasKey() const
{
    return kind == Kind::Button || kind == Kind::Skill || kind == Kind::Swipe
        || kind == Kind::Macro;
}

QJsonObject Node::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("comment"), comment); // serde 无 skip：始终输出
    switch (kind) {
    case Kind::Joystick:
        o.insert(QStringLiteral("type"), QStringLiteral("joystick"));
        o.insert(QStringLiteral("x"), x);
        o.insert(QStringLiteral("y"), y);
        o.insert(QStringLiteral("radius"), radius);
        o.insert(QStringLiteral("up"), up);
        o.insert(QStringLiteral("down"), down);
        o.insert(QStringLiteral("left"), left);
        o.insert(QStringLiteral("right"), right);
        break;
    case Kind::Button:
        o.insert(QStringLiteral("type"), QStringLiteral("button"));
        o.insert(QStringLiteral("key"), key);
        o.insert(QStringLiteral("x"), x);
        o.insert(QStringLiteral("y"), y);
        o.insert(QStringLiteral("mode"), buttonModeName(buttonMode));
        o.insert(QStringLiteral("intervalMs"), double(intervalMs));
        break;
    case Kind::Skill:
        o.insert(QStringLiteral("type"), QStringLiteral("skill"));
        o.insert(QStringLiteral("key"), key);
        o.insert(QStringLiteral("x"), x);
        o.insert(QStringLiteral("y"), y);
        o.insert(QStringLiteral("mode"), skillModeName(skillMode));
        o.insert(QStringLiteral("dragRadius"), dragRadius);
        o.insert(QStringLiteral("cancelable"), cancelable);
        break;
    case Kind::Swipe:
        o.insert(QStringLiteral("type"), QStringLiteral("swipe"));
        o.insert(QStringLiteral("key"), key);
        o.insert(QStringLiteral("x"), x);
        o.insert(QStringLiteral("y"), y);
        o.insert(QStringLiteral("x2"), x2);
        o.insert(QStringLiteral("y2"), y2);
        o.insert(QStringLiteral("durationMs"), double(durationMs));
        break;
    case Kind::Wheel:
        o.insert(QStringLiteral("type"), QStringLiteral("wheel"));
        o.insert(QStringLiteral("upX"), upX);
        o.insert(QStringLiteral("upY"), upY);
        o.insert(QStringLiteral("downX"), downX);
        o.insert(QStringLiteral("downY"), downY);
        break;
    case Kind::Macro:
        o.insert(QStringLiteral("type"), QStringLiteral("macro"));
        o.insert(QStringLiteral("key"), key);
        QJsonArray a;
        for (const MacroStep& s : steps)
            a.append(s.toJson());
        o.insert(QStringLiteral("steps"), a);
        break;
    }
    return o;
}

// ===== Profile =====

QJsonObject Profile::toJson() const
{
    QJsonObject o;
    o.insert(QStringLiteral("version"), double(version));
    o.insert(QStringLiteral("name"), name);
    o.insert(QStringLiteral("package"), package);
    o.insert(QStringLiteral("switchKey"), switchKey);
    o.insert(QStringLiteral("mouseMode"), mouseModeName(mouseMode));
    if (hasLook) // None 不序列化（serde skip_serializing_if）
        o.insert(QStringLiteral("look"), look.toJson());
    if (hasActivity)
        o.insert(QStringLiteral("activity"), activity);
    QJsonArray a;
    for (const Node& n : nodes)
        a.append(n.toJson());
    o.insert(QStringLiteral("nodes"), a);
    return o;
}

mw::Result<Profile> Profile::parse(const QJsonObject& v, QPair<int, int> fallbackLcd)
{
    return parse(QJsonValue(v), fallbackLcd);
}

mw::Result<Profile> Profile::parse(const QByteArray& json, QPair<int, int> fallbackLcd)
{
    const auto doc = QJsonDocument::fromJson(json);
    if (doc.isNull())
        return mw::err<Profile>(QStringLiteral("键位档案格式错误: JSON 解析失败"));
    if (doc.isArray())
        return parse(QJsonValue(doc.array()), fallbackLcd);
    return parse(QJsonValue(doc.object()), fallbackLcd);
}

mw::Result<Profile> Profile::parse(const QJsonValue& v, QPair<int, int> fallbackLcd)
{
    if (v.isArray())
        return migrateV1(v.toArray(), fallbackLcd);
    if (!v.isObject())
        return mw::err<Profile>(
            QStringLiteral("键位档案格式错误: 顶层需为对象（v2）或数组（v1）"));
    Profile p;
    QString err;
    if (!profileFromJson(v.toObject(), &p, &err))
        return mw::err<Profile>(err);
    if (p.version > kSchemaVersion)
        return mw::err<Profile>(QStringLiteral("键位档案版本 %1 不受支持").arg(p.version));
    p.version = kSchemaVersion;
    p.sanitize();
    return mw::Result<Profile>::Ok(p);
}

void Profile::sanitize()
{
    auto clamp01 = [](double& v) { v = std::clamp(v, 0.0, 1.0); };
    for (Node& n : nodes) {
        switch (n.kind) {
        case Node::Kind::Joystick:
            clamp01(n.x);
            clamp01(n.y);
            if (!std::isfinite(n.radius) || n.radius <= 0.0)
                n.radius = 0.16;
            n.radius = std::min(n.radius, 0.5);
            break;
        case Node::Kind::Button:
            clamp01(n.x);
            clamp01(n.y);
            n.intervalMs = std::max<quint32>(n.intervalMs, 40);
            break;
        case Node::Kind::Skill:
            clamp01(n.x);
            clamp01(n.y);
            if (!std::isfinite(n.dragRadius) || n.dragRadius <= 0.0)
                n.dragRadius = 0.18;
            n.dragRadius = std::min(n.dragRadius, 0.5);
            break;
        case Node::Kind::Swipe:
            clamp01(n.x);
            clamp01(n.y);
            clamp01(n.x2);
            clamp01(n.y2);
            n.durationMs = std::max<quint32>(n.durationMs, 30);
            break;
        case Node::Kind::Wheel:
            clamp01(n.upX);
            clamp01(n.upY);
            clamp01(n.downX);
            clamp01(n.downY);
            break;
        case Node::Kind::Macro:
            for (MacroStep& s : n.steps) {
                if (s.action == MacroStep::Action::Tap
                    || s.action == MacroStep::Action::TouchDown) {
                    clamp01(s.x);
                    clamp01(s.y);
                } else if (s.action == MacroStep::Action::Swipe) {
                    clamp01(s.x);
                    clamp01(s.y);
                    clamp01(s.x2);
                    clamp01(s.y2);
                }
            }
            break;
        }
    }
}

} // namespace mw::keymap
