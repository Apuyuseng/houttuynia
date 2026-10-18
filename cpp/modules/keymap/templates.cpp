// 内置键位模板（移植自 crates/mwine-keymap/src/templates.rs；归一化坐标，横屏基准，
// 编辑器里可继续拖拽微调。布局参考 QtScrcpy 社区实战配置 + MuMu/雷电默认键位习惯）。
#include "keymap.hpp"

namespace mw::keymap {
namespace {

// 王者荣耀：WASD 摇杆 + 鼠标方向施法 + 普攻连点。
const char* kHonorOfKings = R"JSON({
    "version": 2,
    "name": "王者荣耀",
    "switchKey": "Grave",
    "mouseMode": "free",
    "nodes": [
        {"type":"joystick","comment":"移动摇杆","x":0.12,"y":0.76,"radius":0.17,
         "up":"W","down":"S","left":"A","right":"D"},
        {"type":"skill","comment":"技能1（鼠标方向施放）","key":"Q","x":0.86,"y":0.90,"mode":"aim","dragRadius":0.18,"cancelable":true},
        {"type":"skill","comment":"技能2","key":"E","x":0.77,"y":0.96,"mode":"aim","dragRadius":0.18,"cancelable":true},
        {"type":"skill","comment":"大招","key":"R","x":0.66,"y":0.98,"mode":"aim","dragRadius":0.18,"cancelable":true},
        {"type":"skill","comment":"召唤师技能","key":"F","x":0.55,"y":0.99,"mode":"quick"},
        {"type":"button","comment":"普攻（按住连击）","key":"MouseLeft","x":0.93,"y":0.80,"mode":"repeat","intervalMs":150},
        {"type":"button","comment":"补刀","key":"T","x":0.845,"y":0.68,"mode":"tap"},
        {"type":"button","comment":"推塔","key":"Y","x":0.855,"y":0.58,"mode":"tap"},
        {"type":"button","comment":"回城","key":"B","x":0.045,"y":0.86,"mode":"tap"},
        {"type":"button","comment":"恢复","key":"G","x":0.045,"y":0.95,"mode":"tap"},
        {"type":"button","comment":"装备商店","key":"I","x":0.965,"y":0.06,"mode":"tap"}
    ]
})JSON";

// 和平精英：WASD + 鼠标捕获视角 + 左键开火右键开镜 + 探头双击切换。
const char* kPeaceElite = R"JSON({
    "version": 2,
    "name": "和平精英",
    "switchKey": "Grave",
    "mouseMode": "capture",
    "activity": "com.epicgames.ue4.GameActivity",
    "look": {"x":0.57,"y":0.26,"sensX":3.25,"sensY":1.25},
    "nodes": [
        {"type":"joystick","comment":"移动摇杆","x":0.16,"y":0.75,"radius":0.18},
        {"type":"button","comment":"开火（按住）","key":"MouseLeft","x":0.86,"y":0.72,"mode":"hold"},
        {"type":"button","comment":"开镜（按住）","key":"MouseRight","x":0.96,"y":0.52,"mode":"hold"},
        {"type":"button","comment":"小眼睛（按住环顾）","key":"OptionLeft","x":0.80,"y":0.31,"mode":"hold"},
        {"type":"button","comment":"左探头（切换）","key":"Q","x":0.12,"y":0.35,"mode":"toggle"},
        {"type":"button","comment":"右探头（切换）","key":"E","x":0.20,"y":0.35,"mode":"toggle"},
        {"type":"button","comment":"跳","key":"Space","x":0.96,"y":0.70,"mode":"tap"},
        {"type":"button","comment":"蹲","key":"C","x":0.86,"y":0.92,"mode":"tap"},
        {"type":"button","comment":"趴","key":"Z","x":0.95,"y":0.90,"mode":"tap"},
        {"type":"button","comment":"换弹","key":"R","x":0.795,"y":0.93,"mode":"tap"},
        {"type":"button","comment":"自动跑","key":"Equals","x":0.84,"y":0.26,"mode":"tap"},
        {"type":"button","comment":"捡东西1","key":"F","x":0.70,"y":0.34,"mode":"tap"},
        {"type":"button","comment":"捡东西2","key":"G","x":0.70,"y":0.44,"mode":"tap"},
        {"type":"button","comment":"开门","key":"X","x":0.70,"y":0.70,"mode":"tap"},
        {"type":"button","comment":"舔包","key":"T","x":0.72,"y":0.26,"mode":"tap"},
        {"type":"button","comment":"视角切换","key":"V","x":0.23,"y":0.95,"mode":"tap"},
        {"type":"button","comment":"地图","key":"M","x":0.98,"y":0.03,"mode":"tap"},
        {"type":"button","comment":"背包","key":"Tab","x":0.06,"y":0.90,"mode":"tap"},
        {"type":"wheel","comment":"滚轮切枪","upX":0.88,"upY":0.62,"downX":0.88,"downY":0.70}
    ]
})JSON";

} // namespace

QVector<QString> templateIds()
{
    return { QStringLiteral("honor_of_kings"), QStringLiteral("peace_elite"),
             QStringLiteral("blank") };
}

QString templateName(const QString& id)
{
    if (id == QLatin1String("honor_of_kings"))
        return QStringLiteral("王者荣耀（MOBA·鼠标瞄准）");
    if (id == QLatin1String("peace_elite"))
        return QStringLiteral("和平精英（FPS·鼠标捕获）");
    if (id == QLatin1String("blank"))
        return QStringLiteral("空白档");
    return QStringLiteral("未知模板");
}

mw::Result<Profile> templateProfile(const QString& id)
{
    QByteArray json;
    if (id == QLatin1String("honor_of_kings"))
        json = QByteArray(kHonorOfKings);
    else if (id == QLatin1String("peace_elite"))
        json = QByteArray(kPeaceElite);
    else if (id == QLatin1String("blank"))
        json = QByteArrayLiteral("{\"version\":2}");
    else
        return mw::err<Profile>(QStringLiteral("未知模板: %1").arg(id));

    // 模板制作基准：1280x720 横屏（归一化坐标，screen 仅 v1 迁移用）
    auto r = Profile::parse(json, qMakePair(1280, 720));
    if (!r)
        return r;
    r.value().version = kSchemaVersion;
    return r;
}

} // namespace mw::keymap
