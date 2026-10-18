// mw::keymap —— 游戏按键映射 v2 数据模型 + 内置模板。
// 移植自 crates/mwine-keymap/src/lib.rs + templates.rs（engine.rs 为 Rust 侧
// 运行时注入引擎，不属本模块）。
// 序列化字段名与 Rust serde 完全一致（camelCase + "type"/"action" tag），
// 保证内置模板 JSON 与已存档 ~/.mwine/keymaps/*.json 双向兼容。
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QPair>
#include <QString>
#include <QVector>

#include <array>

#include "result.hpp"

namespace mw::keymap {

/// 键位档案 schema 版本（对应 Rust SCHEMA_VERSION）。
inline constexpr quint32 kSchemaVersion = 2;

/// 触摸动作（AMOTION_EVENT_ACTION_DOWN/MOVE/UP）。
enum class TouchAction { Down, Move, Up };

/// 鼠标模式：Free（MOBA 类：光标自由移动，方向技能按光标位置瞄准）/
/// Capture（FPS 类：捕获鼠标增量驱动视角拖拽，光标冻结）。
enum class MouseMode { Free, Capture };

/// 按键行为：Tap 点按 / Hold 按住 / Repeat 按住连点 / Toggle 双击切换（探头类）。
enum class ButtonMode { Tap, Hold, Repeat, Toggle };

/// 技能施法：Quick 快速施法 / Aim 方向施法（按住拖拽瞄准，松手施放）。
enum class SkillMode { Quick, Aim };

/// FPS 视角拖拽配置。
struct LookConfig {
    /// 落指锚点（归一化）
    double x = 0.0;
    double y = 0.0;
    /// 增益：宿主像素增量 × sens / 屏幕宽(高) = 归一化位移
    double sensX = 3.2;
    double sensY = 2.0;
    /// 拖拽活动区 [x1,y1,x2,y2]，指针越界时抬指回到锚点（回绕）；无 → boundsArr() 默认区
    bool hasBounds = false;
    std::array<double, 4> bounds{};

    /// 生效活动区（无自定义时给默认 [0.3, 0.05, 0.98, 0.95]）。
    std::array<double, 4> boundsArr() const;
    QJsonObject toJson() const;
};

/// 宏步骤。delayMs 为该步骤执行完后的等待。
struct MacroStep {
    enum class Action { Tap, TouchDown, TouchUp, Swipe, Delay };

    Action action = Action::Tap;
    double x = 0.0;
    double y = 0.0;
    double x2 = 0.0;           // Swipe 终点
    double y2 = 0.0;
    quint32 durationMs = 200;  // Swipe 插值时长
    quint32 delayMs = 0;       // 步骤后等待
    quint32 ms = 0;            // Delay 时长

    QJsonObject toJson() const;
};

/// 映射节点：Rust enum Node（serde tag "type"）→ 带 Kind 判别的扁平结构，
/// 字段按 Kind 取用（未用到的字段保持默认值，不参与该类型的序列化）。
/// 所有坐标归一化到 [0,1]，半径/拖拽距离相对屏幕短边。
struct Node {
    enum class Kind { Joystick, Button, Skill, Swipe, Wheel, Macro };

    Kind kind = Kind::Button;

    /// 所有类型共有
    QString comment;

    // Joystick：中心 + 半径 + 四向键（WASD → 中心按下 + 8 向拖拽）
    double x = 0.0;   // Joystick/Button/Skill/Swipe 起点
    double y = 0.0;
    double radius = 0.16;
    QString up = QStringLiteral("W");
    QString down = QStringLiteral("S");
    QString left = QStringLiteral("A");
    QString right = QStringLiteral("D");

    // Button / Skill / Swipe / Macro 绑定的物理键
    QString key;
    ButtonMode buttonMode = ButtonMode::Tap;
    quint32 intervalMs = 120;   // Button 连点间隔
    SkillMode skillMode = SkillMode::Quick;
    double dragRadius = 0.18;   // Skill 方向施法拖拽半径
    bool cancelable = true;     // Skill 可拖回取消

    // Swipe 终点 / 插值时长
    double x2 = 0.0;
    double y2 = 0.0;
    quint32 durationMs = 200;

    // Wheel：上/下滚 → 点击指定位置（切枪等）
    double upX = 0.0;
    double upY = 0.0;
    double downX = 0.0;
    double downY = 0.0;

    // Macro：按键触发的动作序列
    QVector<MacroStep> steps;

    /// 节点中文类型名（编辑器展示用）。
    QString kindName() const;
    /// 是否绑定单一物理键（摇杆/滚轮无单一键，对应 Rust Node::key() 的 None）。
    bool hasKey() const;

    QJsonObject toJson() const;
};

/// 键位档案 v2。
struct Profile {
    quint32 version = kSchemaVersion;
    QString name;
    QString package;
    /// 总开关键（切换映射开/关；FPS 模式同时退出鼠标捕获）
    QString switchKey = QStringLiteral("Grave");
    MouseMode mouseMode = MouseMode::Free;
    /// FPS 视角配置（mouseMode == Capture 时使用；None → 不序列化）
    bool hasLook = false;
    LookConfig look;
    /// 限定生效的 Activity（包含匹配，如 "com.epicgames.ue4.GameActivity"）；
    /// 游戏登录/选服界面不触发映射，只有对局页激活。None → 不序列化。
    bool hasActivity = false;
    QString activity;
    QVector<Node> nodes;

    /// 解析 v2 对象：缺字段取默认、未知字段忽略；版本 > 2 拒绝；越界坐标自动夹紧。
    static mw::Result<Profile> parse(const QJsonObject& v, QPair<int, int> fallbackLcd);
    /// 解析任意历史格式：v2 对象，或 v1 扁平数组（MWineKeys/MWineUI 旧版，
    /// fallbackLcd 为制作时的 Android 分辨率，用于像素坐标归一化）。
    static mw::Result<Profile> parse(const QJsonValue& v, QPair<int, int> fallbackLcd);
    /// 便捷：直接解析 JSON 文本（顶层对象或数组）。
    static mw::Result<Profile> parse(const QByteArray& json, QPair<int, int> fallbackLcd);

    QJsonObject toJson() const;

    /// 越界坐标夹紧、异常数值回默认（parse 已内置调用；编辑器改档后可手动再调）。
    void sanitize();
};

// ===== 内置模板（templates.rs）=====

/// 内置模板 id 列表（编辑器选择列表用）。
QVector<QString> templateIds();

/// 模板显示名；未知 id 返回 "未知模板"。
QString templateName(const QString& id);

/// 按模板 id 返回档案；id 不存在返回 Err。
mw::Result<Profile> templateProfile(const QString& id);

} // namespace mw::keymap
