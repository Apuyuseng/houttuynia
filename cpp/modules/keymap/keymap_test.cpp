// mw_keymap 自测：模板 / v2 parse-toJson 往返 / v1 迁移 / 夹紧 / 错误路径。
// 注意：不用 <cassert>（Release 下 NDEBUG 会使 assert 失效），自定义 CHECK。
#include <QByteArray>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QtGlobal>

#include <cstdio>

#include "keymap.hpp"

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

static bool near(double a, double b) { return qAbs(a - b) < 1e-9; }

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    using namespace mw::keymap;
    const QPair<int, int> lcd = qMakePair(1280, 720);

    // ===== 模板元信息 =====
    const QVector<QString> ids = templateIds();
    CHECK(ids.size() == 3);
    CHECK(ids.contains(QStringLiteral("honor_of_kings")));
    CHECK(ids.contains(QStringLiteral("peace_elite")));
    CHECK(ids.contains(QStringLiteral("blank")));
    CHECK(templateName(QStringLiteral("honor_of_kings")) == QStringLiteral("王者荣耀（MOBA·鼠标瞄准）"));
    CHECK(templateName(QStringLiteral("peace_elite")) == QStringLiteral("和平精英（FPS·鼠标捕获）"));
    CHECK(templateName(QStringLiteral("blank")) == QStringLiteral("空白档"));
    CHECK(templateName(QStringLiteral("nope")) == QStringLiteral("未知模板"));
    CHECK(!templateProfile(QStringLiteral("nope")).ok());
    CHECK(templateProfile(QStringLiteral("nope")).error().contains(QStringLiteral("未知模板")));

    // ===== 模板全部可解析 + 往返 =====
    for (const QString& id : ids) {
        auto r = templateProfile(id);
        CHECK(r.ok());
        if (!r.ok())
            continue;
        CHECK(r.value().version == kSchemaVersion);
        auto rt = Profile::parse(r.value().toJson(), qMakePair(1600, 900));
        CHECK(rt.ok());
        if (rt.ok())
            CHECK(rt.value().nodes.size() == r.value().nodes.size());
    }

    // 王者荣耀：11 节点，摇杆 + aim 技能 + repeat 普攻
    {
        auto r = templateProfile(QStringLiteral("honor_of_kings"));
        CHECK(r.ok());
        const Profile& p = r.value();
        CHECK(p.nodes.size() == 11);
        CHECK(p.mouseMode == MouseMode::Free);
        CHECK(!p.hasLook);
        CHECK(p.nodes[0].kind == Node::Kind::Joystick);
        CHECK(near(p.nodes[0].x, 0.12) && near(p.nodes[0].y, 0.76));
        CHECK(near(p.nodes[0].radius, 0.17));
        CHECK(p.nodes[0].up == QStringLiteral("W") && p.nodes[0].down == QStringLiteral("S"));
        CHECK(p.nodes[0].left == QStringLiteral("A") && p.nodes[0].right == QStringLiteral("D"));
        bool hasAim = false, hasRepeat = false;
        for (const Node& n : p.nodes) {
            if (n.kind == Node::Kind::Skill && n.skillMode == SkillMode::Aim)
                hasAim = true;
            if (n.kind == Node::Kind::Button && n.buttonMode == ButtonMode::Repeat
                && n.key == QStringLiteral("MouseLeft") && n.intervalMs == 150)
                hasRepeat = true;
        }
        CHECK(hasAim);
        CHECK(hasRepeat);
    }

    // 和平精英：capture + look + activity + hold/toggle/wheel
    {
        auto r = templateProfile(QStringLiteral("peace_elite"));
        CHECK(r.ok());
        const Profile& p = r.value();
        CHECK(p.mouseMode == MouseMode::Capture);
        CHECK(p.hasLook);
        CHECK(near(p.look.x, 0.57) && near(p.look.y, 0.26));
        CHECK(near(p.look.sensX, 3.25) && near(p.look.sensY, 1.25));
        CHECK(!p.look.hasBounds);
        CHECK(p.hasActivity);
        CHECK(p.activity == QStringLiteral("com.epicgames.ue4.GameActivity"));
        bool fireHold = false, scopeHold = false, toggle = false, wheel = false;
        for (const Node& n : p.nodes) {
            if (n.kind == Node::Kind::Button && n.buttonMode == ButtonMode::Hold
                && n.key == QStringLiteral("MouseLeft"))
                fireHold = true;
            if (n.kind == Node::Kind::Button && n.buttonMode == ButtonMode::Hold
                && n.key == QStringLiteral("MouseRight"))
                scopeHold = true;
            if (n.kind == Node::Kind::Button && n.buttonMode == ButtonMode::Toggle)
                toggle = true;
            if (n.kind == Node::Kind::Wheel)
                wheel = true;
        }
        CHECK(fireHold && scopeHold && toggle && wheel);
    }

    // 空白档：默认 Profile
    {
        auto r = templateProfile(QStringLiteral("blank"));
        CHECK(r.ok());
        const Profile& p = r.value();
        CHECK(p.version == 2);
        CHECK(p.name.isEmpty() && p.package.isEmpty());
        CHECK(p.switchKey == QStringLiteral("Grave"));
        CHECK(p.mouseMode == MouseMode::Free);
        CHECK(!p.hasLook && !p.hasActivity);
        CHECK(p.nodes.isEmpty());
        CHECK(p.toJson().value(QStringLiteral("nodes")).toArray().size() == 0);
    }

    // ===== v2：7 种节点往返（用例同 Rust v2_roundtrip）=====
    {
        const QByteArray json = R"({
            "version": 2,
            "name": "测试",
            "switchKey": "Grave",
            "mouseMode": "capture",
            "look": {"x": 0.57, "y": 0.26},
            "nodes": [
                {"type":"joystick","x":0.16,"y":0.75,"radius":0.18},
                {"type":"button","key":"Space","x":0.96,"y":0.7,"mode":"tap"},
                {"type":"button","key":"MouseLeft","x":0.86,"y":0.72,"mode":"hold"},
                {"type":"skill","key":"Q","x":0.8,"y":0.9,"mode":"aim","dragRadius":0.2},
                {"type":"swipe","key":"X","x":0.1,"y":0.1,"x2":0.2,"y2":0.2,"durationMs":150},
                {"type":"wheel","upX":0.87,"upY":0.62,"downX":0.87,"downY":0.68},
                {"type":"macro","key":"F1","steps":[{"action":"tap","x":0.5,"y":0.5,"delayMs":80}]}
            ]
        })";
        auto r = Profile::parse(json, lcd);
        CHECK(r.ok());
        if (r.ok()) {
            const Profile& p = r.value();
            CHECK(p.nodes.size() == 7);
            CHECK(p.name == QStringLiteral("测试"));
            CHECK(p.mouseMode == MouseMode::Capture);
            CHECK(p.hasLook && near(p.look.x, 0.57) && near(p.look.sensX, 3.2)); // sensX 默认
            // 摇杆：四向键缺省 → WASD
            CHECK(p.nodes[0].kind == Node::Kind::Joystick);
            CHECK(near(p.nodes[0].radius, 0.18));
            CHECK(p.nodes[0].up == QStringLiteral("W"));
            // Space tap：intervalMs 缺省 → 120
            CHECK(p.nodes[1].kind == Node::Kind::Button);
            CHECK(p.nodes[1].key == QStringLiteral("Space"));
            CHECK(p.nodes[1].buttonMode == ButtonMode::Tap);
            CHECK(p.nodes[1].intervalMs == 120);
            CHECK(p.nodes[1].comment.isEmpty());
            CHECK(p.nodes[1].hasKey());
            // MouseLeft hold
            CHECK(p.nodes[2].buttonMode == ButtonMode::Hold);
            // 技能：cancelable 缺省 → true
            CHECK(p.nodes[3].kind == Node::Kind::Skill);
            CHECK(p.nodes[3].skillMode == SkillMode::Aim);
            CHECK(near(p.nodes[3].dragRadius, 0.2));
            CHECK(p.nodes[3].cancelable == true);
            // 滑动
            CHECK(p.nodes[4].kind == Node::Kind::Swipe);
            CHECK(p.nodes[4].durationMs == 150);
            CHECK(near(p.nodes[4].x2, 0.2) && near(p.nodes[4].y2, 0.2));
            // 滚轮：无单一键
            CHECK(p.nodes[5].kind == Node::Kind::Wheel);
            CHECK(!p.nodes[5].hasKey());
            CHECK(near(p.nodes[5].upX, 0.87) && near(p.nodes[5].downY, 0.68));
            // 宏
            CHECK(p.nodes[6].kind == Node::Kind::Macro);
            CHECK(p.nodes[6].steps.size() == 1);
            CHECK(p.nodes[6].steps[0].action == MacroStep::Action::Tap);
            CHECK(p.nodes[6].steps[0].delayMs == 80);
            CHECK(near(p.nodes[6].steps[0].x, 0.5));

            // 序列化往返：字段名/值稳定
            const QJsonObject out = p.toJson();
            CHECK(out.value(QStringLiteral("mouseMode")).toString() == QStringLiteral("capture"));
            CHECK(out.value(QStringLiteral("switchKey")).toString() == QStringLiteral("Grave"));
            CHECK(out.contains(QStringLiteral("look")));
            CHECK(out.value(QStringLiteral("nodes")).toArray().size() == 7);
            const QJsonObject btn = out.value(QStringLiteral("nodes")).toArray().at(1).toObject();
            CHECK(btn.value(QStringLiteral("type")).toString() == QStringLiteral("button"));
            CHECK(btn.contains(QStringLiteral("intervalMs"))); // serde 无 skip：默认值也输出
            CHECK(btn.contains(QStringLiteral("comment")));
            auto r2 = Profile::parse(out, lcd);
            CHECK(r2.ok());
            if (r2.ok())
                CHECK(r2.value().toJson() == out); // 幂等
        }
    }

    // 契约入口：QJsonObject 直接 parse
    {
        QJsonObject o;
        o.insert(QStringLiteral("version"), 2.0);
        o.insert(QStringLiteral("name"), QStringLiteral("obj"));
        auto r = Profile::parse(o, lcd);
        CHECK(r.ok());
        if (r.ok())
            CHECK(r.value().name == QStringLiteral("obj"));
    }

    // ===== v1 迁移（用例同 Rust v1_migration）=====
    {
        const QByteArray v1 = R"([
            {"key":"w","type":"tap","x":640,"y":360,"interval":0},
            {"key":"space","type":"hold","x":100,"y":600,"interval":90},
            {"key":"x","type":"swipe","x":10,"y":20,"x2":30,"y2":40,"duration":250}
        ])";
        auto r = Profile::parse(v1, lcd);
        CHECK(r.ok());
        if (r.ok()) {
            const Profile& p = r.value();
            CHECK(p.nodes.size() == 3);
            CHECK(p.version == 2);
            CHECK(p.switchKey == QStringLiteral("Grave"));
            CHECK(p.nodes[0].kind == Node::Kind::Button);
            CHECK(p.nodes[0].key == QStringLiteral("W")); // 单字母大写
            CHECK(near(p.nodes[0].x, 0.5) && near(p.nodes[0].y, 0.5));
            CHECK(p.nodes[0].buttonMode == ButtonMode::Tap);
            CHECK(p.nodes[0].intervalMs == 120); // tap 分支用默认间隔（不取 v1 interval）
            CHECK(p.nodes[1].key == QStringLiteral("Space")); // 特殊键映射
            CHECK(p.nodes[1].buttonMode == ButtonMode::Repeat);
            CHECK(p.nodes[1].intervalMs == 90);
            CHECK(p.nodes[2].kind == Node::Kind::Swipe);
            CHECK(near(p.nodes[2].x, 10.0 / 1280.0) && near(p.nodes[2].y2, 40.0 / 720.0));
            CHECK(p.nodes[2].durationMs == 250);
        }
        // v1 swipe 缺省 x2 → x+300 归一化；duration 下限 30
        const QByteArray v1b = R"([{"key":"x","type":"swipe","x":100,"y":200,"duration":10}])";
        auto rb = Profile::parse(v1b, lcd);
        CHECK(rb.ok());
        if (rb.ok()) {
            const Node& n = rb.value().nodes[0];
            CHECK(near(n.x2, 400.0 / 1280.0));
            CHECK(near(n.y2, 200.0 / 720.0));
            CHECK(n.durationMs == 30);
        }
    }

    // ===== 夹紧 / 异常数值回默认（sanitize）=====
    {
        auto parseOne = [lcd](const char* nodeJson) {
            const QByteArray j = QByteArrayLiteral("{\"nodes\":[") + nodeJson + "]}";
            return Profile::parse(j, lcd);
        };
        auto c = parseOne(R"({"type":"button","key":"J","x":1.7,"y":-0.3})");
        CHECK(c.ok());
        if (c.ok()) {
            CHECK(near(c.value().nodes[0].x, 1.0));
            CHECK(near(c.value().nodes[0].y, 0.0));
        }
        auto j = parseOne(R"({"type":"joystick","x":0.5,"y":0.5,"radius":0.9})");
        CHECK(j.ok() && near(j.value().nodes[0].radius, 0.5)); // 上限 0.5
        auto j2 = parseOne(R"({"type":"joystick","x":0.5,"y":0.5,"radius":-1})");
        CHECK(j2.ok() && near(j2.value().nodes[0].radius, 0.16)); // 非法 → 默认
        auto s = parseOne(R"({"type":"skill","key":"Q","x":0.5,"y":0.5,"dragRadius":2})");
        CHECK(s.ok() && near(s.value().nodes[0].dragRadius, 0.5));
        auto sw = parseOne(R"({"type":"swipe","key":"X","x":0.1,"y":0.1,"x2":0.2,"y2":0.2,"durationMs":5})");
        CHECK(sw.ok() && sw.value().nodes[0].durationMs == 30); // 下限 30
        auto bt = parseOne(R"({"type":"button","key":"B","x":0.5,"y":0.5,"intervalMs":5})");
        CHECK(bt.ok() && bt.value().nodes[0].intervalMs == 40); // 下限 40
        auto mc = parseOne(R"({"type":"macro","key":"M","steps":[{"action":"tap","x":2,"y":-1}]})");
        CHECK(mc.ok());
        if (mc.ok()) {
            CHECK(near(mc.value().nodes[0].steps[0].x, 1.0));
            CHECK(near(mc.value().nodes[0].steps[0].y, 0.0));
        }
    }

    // ===== look.bounds：可选 + 默认区 =====
    {
        const QByteArray withBounds =
            R"({"version":2,"look":{"x":0.5,"y":0.5,"bounds":[0.2,0.1,0.9,0.9]}})";
        auto r = Profile::parse(withBounds, lcd);
        CHECK(r.ok());
        if (r.ok()) {
            const LookConfig& l = r.value().look;
            CHECK(l.hasBounds);
            CHECK(near(l.boundsArr()[0], 0.2) && near(l.boundsArr()[3], 0.9));
            const QJsonObject lo = r.value().toJson().value(QStringLiteral("look")).toObject();
            CHECK(lo.contains(QStringLiteral("bounds")));
            CHECK(lo.contains(QStringLiteral("sensX"))); // 默认值也序列化
        }
        // 无 bounds → 不序列化，boundsArr 给默认
        const QByteArray noBounds = R"({"version":2,"look":{"x":0.5,"y":0.5}})";
        auto r2 = Profile::parse(noBounds, lcd);
        CHECK(r2.ok());
        if (r2.ok()) {
            CHECK(!r2.value().look.hasBounds);
            const auto d = r2.value().look.boundsArr();
            CHECK(near(d[0], 0.3) && near(d[1], 0.05) && near(d[2], 0.98) && near(d[3], 0.95));
            const QJsonObject lo = r2.value().toJson().value(QStringLiteral("look")).toObject();
            CHECK(!lo.contains(QStringLiteral("bounds")));
        }
        // None 语义：look/activity 显式 null → 视为缺省，且不序列化
        const QByteArray nulls = R"({"version":2,"look":null,"activity":null})";
        auto r3 = Profile::parse(nulls, lcd);
        CHECK(r3.ok());
        if (r3.ok()) {
            CHECK(!r3.value().hasLook && !r3.value().hasActivity);
            CHECK(!r3.value().toJson().contains(QStringLiteral("look")));
        }
    }

    // ===== 错误路径 =====
    {
        // 版本过高
        auto v = Profile::parse(QByteArrayLiteral("{\"version\":3}"), lcd);
        CHECK(v.isErr());
        CHECK(v.error().contains(QStringLiteral("不受支持")));
        // 缺必填字段
        auto m = Profile::parse(
            QByteArrayLiteral(R"({"nodes":[{"type":"button","key":"J"}]})"), lcd);
        CHECK(m.isErr());
        CHECK(m.error().contains(QStringLiteral("nodes[0]")));
        // 未知节点类型
        auto u = Profile::parse(
            QByteArrayLiteral(R"({"nodes":[{"type":"laser","key":"J"}]})"), lcd);
        CHECK(u.isErr());
        // 未知按键模式
        auto b = Profile::parse(QByteArrayLiteral(
                                    R"({"nodes":[{"type":"button","key":"J","x":0.5,"y":0.5,"mode":"burst"}]})"),
                                lcd);
        CHECK(b.isErr());
        // 顶层非法
        auto t = Profile::parse(QByteArrayLiteral("\"str\""), lcd);
        CHECK(t.isErr());
        // look 缺 x
        auto l = Profile::parse(QByteArrayLiteral(R"({"look":{"y":0.5}})"), lcd);
        CHECK(l.isErr());
        // 空对象 → 全默认（版本兼容）
        auto e = Profile::parse(QByteArrayLiteral("{}"), lcd);
        CHECK(e.ok());
        if (e.ok()) {
            CHECK(e.value().version == 2);
            CHECK(e.value().switchKey == QStringLiteral("Grave"));
            CHECK(e.value().nodes.isEmpty());
        }
    }

    // ===== 类型名（编辑器展示）=====
    {
        Node n;
        n.kind = Node::Kind::Joystick;
        CHECK(n.kindName() == QStringLiteral("摇杆"));
        n.kind = Node::Kind::Macro;
        CHECK(n.kindName() == QStringLiteral("宏"));
        n.kind = Node::Kind::Wheel;
        CHECK(n.kindName() == QStringLiteral("滚轮"));
    }

    if (g_failures == 0) {
        std::puts("mw_keymap_test: ALL PASSED");
        return 0;
    }
    std::printf("mw_keymap_test: %d FAILURES\n", g_failures);
    return 1;
}
