// mw::config —— 虚拟机配置 VmConfig（~/.mwine/config.json）。
// 移植自 crates/mwine-core/src/config.rs（daemon 实际使用的字段集；
// crates/mwine-config 内的旧 DeviceConfig/热生效栈不在本模块契约内）。
// JSON 键名保持 Rust serde 原样（snake_case、全字段输出），成员名转 camelCase。
#pragma once

#include <QJsonObject>
#include <QString>

#include "result.hpp"

namespace mw::config {

/// 当前生效的屏幕参数（对应 Rust lcd() -> (u32, u32, u32)）。
struct Lcd {
    int w = 0;
    int h = 0;
    int dpi = 0;
};

struct VmConfig {
    /// phone | tablet | custom（lcd() 另识别 tv）
    QString mode = QStringLiteral("phone");
    /// custom 模式生效的尺寸
    quint32 width = 1080;
    quint32 height = 2400;
    quint32 dpi = 440;
    /// 内存 MB（0 = 镜像默认 2048）
    quint32 memoryMb = 2048;
    /// sdcard 挂载位置（emulator -sdcard 指向的镜像文件路径；空 = AVD 默认）
    QString sdcardPath;
    /// 宿主共享目录（9p 挂载到 guest /mnt/host；空 = 不挂载）
    QString shareDir;
    /// AVD 名
    QString avd = QStringLiteral("MWine4");
    /// guest HTTP 代理（如 http://127.0.0.1:7897；空 = 不代理）
    QString proxy;
    // ===== 专业参数 =====
    /// CPU 核数（0 = 默认 4）
    quint32 cpuCores = 0;
    /// GPU 后端：host / swangle / swiftshader_indirect。默认 host：guest 渲染
    /// 在宿主 Apple GPU 重放（gfxstream→MoltenVK→Metal，Apple M5 实证可用），
    /// guest CPU 零渲染负担；不传 -gpu 的 qemu 默认是 SwiftShader 纯软渲
    /// （ANGLE→Vulkan→SwiftShader，CPU 密集且拖慢一切取帧路径）。
    QString gpuMode = QStringLiteral("host");
    /// QEMU 附加参数（原样追加到 -qemu 透传块；空 = 无）
    QString qemuExtra;
    /// 网络速度模拟（如 gsm / hscsd / full；空 = 不模拟）
    QString netspeed;
    /// 网络延迟模拟（如 gprs / umts / none；空 = 不模拟）
    QString netdelay;
    /// 禁用快照（每次冷启动）
    bool noSnapshot = false;
    /// 禁用启动动画
    bool noBootAnim = false;
    // ===== 摄像头 / 麦克风 =====
    /// 后置摄像头："" = AVD 默认 | none | emulated | virtualscene | webcam0（Mac 摄像头）
    QString cameraBack;
    /// 前置摄像头：取值同上
    QString cameraFront;
    /// 允许应用使用 Mac 麦克风（-allow-host-audio；默认输入静音）
    bool allowHostAudio = false;
    // ===== 网络 =====
    /// 自定义 DNS（-dns-server，如 "223.5.5.5" 或逗号分隔多个；空 = 默认）
    QString dnsServers;
    // ===== 性能 =====
    /// 屏幕刷新率 Hz（-vsync-rate；0 = 默认 60，可选 60/90/120）
    quint32 vsyncRate = 0;
    // ===== 快照行为 =====
    /// 关机时不保存状态（下次从上一个存档启动；区别于 noSnapshot 的完全不碰快照）
    bool noSnapshotSave = false;
    /// 空闲自动关停分钟数（0 = 从不；默认 30）
    quint32 idleStopMin = 30;
    /// 独立窗口模式时隐藏模拟器主窗：开独立窗口即隐藏、关窗后不恢复、
    /// VM 引导完成后保持隐藏；emulator 模式应用启动前自动恢复（勿与
    /// MWINE_KEEP_EMULATOR 混淆——后者是 CLI 级"全程不碰主窗"的调试开关）
    bool hideEmulatorWindow = false;
    /// 独立窗口总开关：开启后所有应用（忽略 appmodes.json 逐应用设置）都
    /// 以独立 macOS 窗口运行，VM 引导后主窗自动隐藏（等效无头）。注意 VM
    /// 不能以 -no-window 启动：qemu 无头会把 multidisplay 副屏帧按主屏尺寸
    /// 渲染（36.5.11 实测），直通窗口被信箱化——故"无头"用隐藏主窗等效。
    /// 切换后重启 VM 生效；hideEmulatorWindow 在其开启时自然失效
    bool windowModeAll = false;
    /// 独立窗口无边框（无标题栏/红绿灯；⌘拖动移动、⌥拖动缩放、Esc/⌘W 关闭）。
    /// 由守护进程以 MWINE_BORDERLESS=1 注入 run --window 子进程
    bool windowBorderless = false;
    // ===== 设备身份 =====
    /// 虚拟手机号（-phone-number；空 = 模拟器默认）
    QString phoneNumber;
    /// 无 SIM 卡模式（-no-sim）
    bool noSim = false;
    /// 自定义 Android 序列号（-android-serialno；空 = 默认）
    QString serialno;
    /// 手动语言覆盖（如 zh-CN；空 = 跟随 Mac 主语言）
    QString locale;
    // ===== 兼容性（-feature 强制关/开）=====
    /// 关闭硬件视频解码（媒体播放/存储异常自救；纯软解更稳）
    bool hwDecoderOff = false;
    /// 关闭 Vulkan（游戏探测 Vulkan 崩溃/花屏时尝试）
    bool vulkanOff = false;
    /// 关闭 GLDMA 直传（视频花屏时尝试）
    bool gldmaOff = false;
    /// 输入回退 goldfish_events（点击/键盘失灵排障）
    bool virtioInputOff = false;
    /// 宿主鼠标事件改为触摸事件（-feature -VirtioMouse；游戏鼠标当手指）
    bool mouseAsTouch = false;
    // ===== 输入（AVD config.ini）=====
    /// Mac 键盘直通 guest（hw.keyboard；关闭后 guest 会弹软键盘）
    bool hwKeyboard = true;
    // ===== 诊断 =====
    /// SELinux 模式："" = 默认 | permissive | disabled
    QString selinux;
    /// 虚拟机网络抓包（-tcpdump → ~/.mwine/logs/capture.pcap）
    bool tcpdump = false;
    /// 检测 guest 卡死（-detect-image-hang，防 offline 僵尸）
    bool detectImageHang = false;
    /// /system 分区可写（-writable-system；userdebug 镜像 adb remount 用）
    bool writableSystem = false;

    /// 读取 ~/.mwine/config.json；缺失/解析失败 → 默认值
    ///（对应 Rust load() 的 read→parse→unwrap_or_default）。
    static VmConfig load();
    /// 原子写（mw::writeFile，pretty JSON）。
    mw::Result<bool> save() const;
    /// 当前生效的 (宽, 高, dpi)：phone/tablet/tv 预设，custom 及未知模式用自定义值。
    Lcd lcd() const;
    /// 集中校验：这些值会拼进 emulator 参数 / guest shell 命令，格式必须收紧
    ///（既有 qemuExtra 是显式的专业透传口，不在校验范围）。规则照抄 config.rs validate()。
    mw::Result<bool> validate() const;
    /// 序列化：键名与 Rust serde 输出一致（snake_case），全字段输出。
    QJsonObject toJson() const;
    /// 反序列化：缺字段取默认（对应 serde default），未知字段忽略。
    static VmConfig fromJson(const QJsonObject& o);

    /// 配置文件路径 ~/.mwine/config.json。
    static QString path();
};

} // namespace mw::config
