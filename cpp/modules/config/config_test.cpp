// mw_config 自测：默认值 / lcd 映射 / validate 规则 / JSON 往返 / 落盘加载。
// 注意：不用 <cassert>（Release 下 NDEBUG 会使 assert 失效），自定义 CHECK。
#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QString>
#include <QTemporaryDir>
#include <QtGlobal>

#include <cstdio>

#include "config.hpp"
#include "fs.hpp"

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
    using mw::config::VmConfig;

    // ===== 默认值 =====
    VmConfig def;
    CHECK(def.mode == QStringLiteral("phone"));
    CHECK(def.width == 1080 && def.height == 2400 && def.dpi == 440);
    CHECK(def.memoryMb == 2048);
    CHECK(def.avd == QStringLiteral("MWine4"));
    CHECK(def.cpuCores == 0);
    CHECK(def.idleStopMin == 30);
    CHECK(def.hwKeyboard == true);
    CHECK(def.mouseAsTouch == false);
    CHECK(def.shareDir.isEmpty() && def.proxy.isEmpty());
    CHECK(def.validate().ok());

    // ===== lcd 映射 =====
    CHECK(def.lcd().w == 1080 && def.lcd().h == 2400 && def.lcd().dpi == 440);
    VmConfig t = def;
    t.mode = QStringLiteral("tablet");
    CHECK(t.lcd().w == 2560 && t.lcd().h == 1600 && t.lcd().dpi == 320);
    t.mode = QStringLiteral("tv");
    CHECK(t.lcd().w == 1920 && t.lcd().h == 1080 && t.lcd().dpi == 320);
    t.mode = QStringLiteral("custom");
    t.width = 1440;
    t.height = 3200;
    t.dpi = 480;
    CHECK(t.lcd().w == 1440 && t.lcd().h == 3200 && t.lcd().dpi == 480);

    // ===== validate 规则（照抄 config.rs validate()）=====
    {
        VmConfig c = def;
        c.cameraBack = QStringLiteral("webcam1");
        CHECK(c.validate().isErr());
        CHECK(c.validate().error().contains(QStringLiteral("后置摄像头")));
        c = def;
        c.cameraFront = QStringLiteral("virtualscene");
        CHECK(c.validate().ok());

        // DNS
        c = def;
        c.dnsServers = QStringLiteral("223.5.5.5, 8.8.8.8"); // 逗号 + 空格
        CHECK(c.validate().ok());
        c.dnsServers = QStringLiteral("1.2.3");
        CHECK(c.validate().isErr());
        c.dnsServers = QStringLiteral("fe80:0:0:0:0:0:0:1"); // 全段十六进制
        CHECK(c.validate().ok());
        // 与 Rust 语义一致：'::' 压缩段为空 → 拒
        c.dnsServers = QStringLiteral("fe80::1");
        CHECK(c.validate().isErr());
        c.dnsServers = QStringLiteral("1.2.3.4,5.6.7");
        CHECK(c.validate().isErr());

        // 刷新率
        c = def;
        c.vsyncRate = 144;
        CHECK(c.validate().isErr());
        c.vsyncRate = 120;
        CHECK(c.validate().ok());
        c.vsyncRate = 90;
        CHECK(c.validate().ok());
        c.vsyncRate = 0;
        CHECK(c.validate().ok());

        // 手机号
        c = def;
        c.phoneNumber = QStringLiteral("+86-13800138000");
        CHECK(c.validate().ok());
        c.phoneNumber = QStringLiteral("13800abc");
        CHECK(c.validate().isErr());

        // 序列号
        c = def;
        c.serialno = QStringLiteral("MW.2026_abc-,");
        CHECK(c.validate().ok());
        c.serialno = QStringLiteral("a b");
        CHECK(c.validate().isErr());
        c.serialno = QStringLiteral("a/b");
        CHECK(c.validate().isErr());

        // 语言
        c = def;
        c.locale = QStringLiteral("zh");
        CHECK(c.validate().ok());
        c.locale = QStringLiteral("zh-CN");
        CHECK(c.validate().ok());
        c.locale = QStringLiteral("ZH"); // 大写 lang 非法
        CHECK(c.validate().isErr());
        c.locale = QStringLiteral("zh-cn"); // 小写 region 非法
        CHECK(c.validate().isErr());
        c.locale = QStringLiteral("zh-CN-x"); // 三段非法
        CHECK(c.validate().isErr());
        c.locale = QStringLiteral("zhcn"); // 长度 4 非法
        CHECK(c.validate().isErr());

        // SELinux
        c = def;
        c.selinux = QStringLiteral("permissive");
        CHECK(c.validate().ok());
        c.selinux = QStringLiteral("disabled");
        CHECK(c.validate().ok());
        c.selinux = QStringLiteral("enforcing");
        CHECK(c.validate().isErr());
    }

    // ===== JSON 往返（全字段 + snake_case 键名）=====
    {
        VmConfig c;
        c.mode = QStringLiteral("custom");
        c.width = 1440;
        c.height = 3200;
        c.dpi = 480;
        c.memoryMb = 4096;
        c.sdcardPath = QStringLiteral("/tmp/sd.img");
        c.shareDir = QStringLiteral("/Users/x/share");
        c.avd = QStringLiteral("TestAVD");
        c.proxy = QStringLiteral("http://127.0.0.1:7897");
        c.cpuCores = 8;
        c.gpuMode = QStringLiteral("host");
        c.qemuExtra = QStringLiteral("-cpu host");
        c.netspeed = QStringLiteral("full");
        c.netdelay = QStringLiteral("none");
        c.noSnapshot = true;
        c.noBootAnim = true;
        c.cameraBack = QStringLiteral("webcam0");
        c.cameraFront = QStringLiteral("none");
        c.allowHostAudio = true;
        c.dnsServers = QStringLiteral("223.5.5.5");
        c.vsyncRate = 90;
        c.noSnapshotSave = true;
        c.idleStopMin = 0;
        c.phoneNumber = QStringLiteral("+8613800138000");
        c.noSim = true;
        c.serialno = QStringLiteral("MW-2026.001");
        c.locale = QStringLiteral("zh-CN");
        c.hwDecoderOff = true;
        c.vulkanOff = true;
        c.gldmaOff = true;
        c.virtioInputOff = true;
        c.mouseAsTouch = true;
        c.hwKeyboard = false;
        c.selinux = QStringLiteral("disabled");
        c.tcpdump = true;
        c.detectImageHang = true;
        c.writableSystem = true;
        CHECK(c.validate().ok());

        const QJsonObject o = c.toJson();
        // 键名与 Rust serde 完全一致（snake_case）
        CHECK(o.contains(QStringLiteral("memory_mb")));
        CHECK(o.contains(QStringLiteral("sdcard_path")));
        CHECK(o.contains(QStringLiteral("cpu_cores")));
        CHECK(o.contains(QStringLiteral("idle_stop_min")));
        CHECK(o.contains(QStringLiteral("hw_keyboard")));
        CHECK(o.contains(QStringLiteral("writable_system")));
        CHECK(!o.contains(QStringLiteral("memoryMb"))); // 不得出现 camelCase 键

        const VmConfig b = VmConfig::fromJson(o);
        CHECK(b.mode == c.mode);
        CHECK(b.width == c.width && b.height == c.height && b.dpi == c.dpi);
        CHECK(b.memoryMb == c.memoryMb);
        CHECK(b.sdcardPath == c.sdcardPath && b.shareDir == c.shareDir);
        CHECK(b.avd == c.avd && b.proxy == c.proxy);
        CHECK(b.cpuCores == c.cpuCores && b.gpuMode == c.gpuMode);
        CHECK(b.qemuExtra == c.qemuExtra && b.netspeed == c.netspeed && b.netdelay == c.netdelay);
        CHECK(b.noSnapshot == true && b.noBootAnim == true);
        CHECK(b.cameraBack == c.cameraBack && b.cameraFront == c.cameraFront);
        CHECK(b.allowHostAudio == true);
        CHECK(b.dnsServers == c.dnsServers);
        CHECK(b.vsyncRate == 90 && b.noSnapshotSave == true && b.idleStopMin == 0);
        CHECK(b.phoneNumber == c.phoneNumber && b.noSim == true);
        CHECK(b.serialno == c.serialno && b.locale == c.locale);
        CHECK(b.hwDecoderOff && b.vulkanOff && b.gldmaOff && b.virtioInputOff);
        CHECK(b.mouseAsTouch == true && b.hwKeyboard == false);
        CHECK(b.selinux == c.selinux);
        CHECK(b.tcpdump && b.detectImageHang && b.writableSystem);
    }

    // ===== 缺字段取默认 / 未知字段忽略 / 空对象 =====
    {
        const VmConfig empty = VmConfig::fromJson(QJsonObject());
        CHECK(empty.mode == QStringLiteral("phone"));
        CHECK(empty.width == 1080 && empty.idleStopMin == 30 && empty.hwKeyboard == true);

        QJsonObject o;
        o.insert(QStringLiteral("mode"), QStringLiteral("custom"));
        o.insert(QStringLiteral("width"), 1234.0);
        o.insert(QStringLiteral("future_field"), 42); // 向前兼容：未知字段忽略
        const VmConfig p = VmConfig::fromJson(o);
        CHECK(p.mode == QStringLiteral("custom"));
        CHECK(p.width == 1234);
        CHECK(p.height == 2400 && p.dpi == 440 && p.memoryMb == 2048);
        CHECK(p.avd == QStringLiteral("MWine4"));
        CHECK(p.hwKeyboard == true && p.idleStopMin == 30);
        CHECK(p.lcd().w == 1234 && p.lcd().h == 2400);
    }

    // ===== save / load（HOME 重定向到临时目录，不碰真实 ~/.mwine）=====
    {
        const QByteArray origHome = qgetenv("HOME");
        QTemporaryDir tmp;
        CHECK(tmp.isValid());
        if (tmp.isValid()) {
            qputenv("HOME", tmp.path().toUtf8());
            CHECK(mw::homeDir() == tmp.path()); // HOME 覆盖生效（失败则后续跳过语义判定）

            // 无文件 → 默认
            const VmConfig none = VmConfig::load();
            CHECK(none.mode == QStringLiteral("phone") && none.width == 1080);

            // 保存 → 加载
            VmConfig c;
            c.mode = QStringLiteral("custom");
            c.width = 1000;
            c.height = 2000;
            c.dpi = 300;
            c.gpuMode = QStringLiteral("host");
            const auto sr = c.save();
            CHECK(sr.ok());
            CHECK(QFile::exists(VmConfig::path()));
            const auto bytes = mw::readFile(VmConfig::path());
            CHECK(bytes.ok());
            CHECK(bytes.value().contains("\"memory_mb\"")); // pretty JSON + snake_case 键
            const VmConfig back = VmConfig::load();
            CHECK(back.mode == QStringLiteral("custom"));
            CHECK(back.width == 1000 && back.height == 2000 && back.dpi == 300);
            CHECK(back.gpuMode == QStringLiteral("host"));

            // 坏 JSON → 默认（对应 Rust unwrap_or_default）
            QFile f(VmConfig::path());
            CHECK(f.open(QIODevice::WriteOnly));
            f.write("not json");
            f.close();
            const VmConfig bad = VmConfig::load();
            CHECK(bad.width == 1080 && bad.mode == QStringLiteral("phone"));

            qputenv("HOME", origHome);
        }
    }

    if (g_failures == 0) {
        std::puts("mw_config_test: ALL PASSED");
        return 0;
    }
    std::printf("mw_config_test: %d FAILURES\n", g_failures);
    return 1;
}
