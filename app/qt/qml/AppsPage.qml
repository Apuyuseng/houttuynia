import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import QtQuick.Effects
import Houttuynia

Item {
    id: page

    // ===== 顶栏状态汇总 =====
    readonly property bool installBusy: backend.installTask.running === true
    readonly property string installErr: backend.installTask.error || ""
    readonly property double installFrac: (backend.installTask.progress_total || 0) > 0
            ? Math.min(1, (backend.installTask.progress || 0) / backend.installTask.progress_total)
            : 0

    readonly property int runningApps: {
        let n = 0
        const a = backend.apps
        for (let i = 0; i < a.length; ++i)
            if (a[i] && a[i].running) n++
        return n
    }
    readonly property int cpuPct: Math.round(backend.perf.cpu_pct || 0)
    readonly property double memUsedMb: backend.perf.mem && backend.perf.mem.total_mb
            ? backend.perf.mem.total_mb - (backend.perf.mem.available_mb || 0) : 0
    readonly property double memTotalMb: backend.perf.mem ? (backend.perf.mem.total_mb || 0) : 0
    readonly property double memPct: memTotalMb > 0 ? memUsedMb / memTotalMb * 100 : 0

    function fmtMB(mb) {
        if (mb >= 1024) {
            const gb = (mb / 1024).toFixed(1)
            return (gb.endsWith(".0") ? gb.slice(0, -2) : gb) + " GB"
        }
        return Math.round(mb) + " MB"
    }

    // 打开「在线下载」独立窗口。
    // 用 window 附着属性取宿主窗 + Qt.globalEventDispatcher 无关——直接调
    // root 暴露的 openStoreWindow。Main 根对象是 ApplicationWindow，经
    // loadFromModule 加载时其类型是 Main，instanceof ApplicationWindow 判断
    // 不可靠（dist 包中曾为 null 导致点击无效），故改为按 window 附着属性找宿主。
    function openStore() {
        const w = page.Window.window
        if (w && typeof w.openStoreWindow === "function") {
            w.openStoreWindow()
        } else {
            console.warn("openStoreWindow: 未找到宿主窗口或函数", w,
                         w && typeof w.openStoreWindow)
        }
    }

    // 状态胶囊：顶栏内的轻量指示（图标/圆点 + 数值），悬停出完整说明。
    // themedIcon 为真时图标按主题文字色着色（灰度 SVG 自适应深浅色）
    component StatPill: Item {
        id: pill
        property string icon: ""                 // 左侧小图标（qrc 路径，与圆点二选一）
        property bool themedIcon: false
        property color dotColor: "transparent"   // 左侧状态圆点
        property string value: ""
        property color valueColor: Theme.text
        property string tip: ""

        implicitHeight: 28
        implicitWidth: row.implicitWidth + 20

        Rectangle {
            anchors.fill: parent
            radius: height / 2
            color: pillHover.hovered
                   ? (Theme.dark ? Qt.rgba(1, 1, 1, 0.10) : Qt.rgba(0, 0, 0, 0.055))
                   : (Theme.dark ? Qt.rgba(1, 1, 1, 0.06) : Qt.rgba(0, 0, 0, 0.035))
            Behavior on color { ColorAnimation { duration: 100 } }
        }

        RowLayout {
            id: row
            anchors.centerIn: parent
            spacing: 6

            Rectangle {
                visible: pill.dotColor.a > 0
                Layout.preferredWidth: 7
                Layout.preferredHeight: 7
                radius: 3.5
                color: pill.dotColor
            }
            Item {
                visible: pill.icon.length > 0
                Layout.preferredWidth: 14
                Layout.preferredHeight: 14
                Image {
                    id: pillIcon
                    anchors.fill: parent
                    source: pill.icon
                    fillMode: Image.PreserveAspectFit
                    sourceSize: Qt.size(28, 28)
                    visible: !pill.themedIcon
                }
                MultiEffect {
                    anchors.fill: pillIcon
                    source: pillIcon
                    visible: pill.themedIcon
                    colorization: 1.0
                    colorizationColor: Theme.text
                }
            }
            Text {
                visible: pill.value.length > 0
                text: pill.value
                font.pixelSize: 13; font.weight: Font.DemiBold
                color: pill.valueColor
            }
        }

        HoverHandler { id: pillHover }
        ToolTip.visible: pillHover.hovered && pill.tip.length > 0
        ToolTip.text: pill.tip
        ToolTip.delay: 350
    }

    // 虚拟机壁纸背景（守护自动抓取缓存；未就绪回退主题底色）。
    // 读失败（如文件正被原子替换的瞬间）自动重试，避免卡在 Error 态不再显示
    Image {
        id: wallpaperBg
        anchors.fill: parent
        property int retries: 0
        property string stamp: ""
        source: backend.appWallpaper.length > 0
                ? "file://" + backend.appWallpaper + "#" + stamp : ""
        fillMode: Image.PreserveAspectCrop
        smooth: true
        visible: status === Image.Ready
        onSourceChanged: retries = 0
        onStatusChanged: {
            if (status === Image.Error && retries < 5) {
                retries++
                retryTimer.restart()
            }
        }
        Timer {
            id: retryTimer
            interval: 2500
            onTriggered: wallpaperBg.stamp = Date.now()
        }
        // 守护侧文件更新（换壁纸）→ stamp 变化强制重载
        Connections {
            target: backend
            function onAppsChanged() {
                if (wallpaperBg.status === Image.Ready
                        || wallpaperBg.retries >= 5)
                    wallpaperBg.stamp = Date.now()
            }
        }
    }

    // 应用网格：延伸到窗口顶边，滚动内容从毛玻璃工具栏下方穿过
    ScrollView {
        visible: backend.vmRunning
        anchors.fill: parent
        anchors.leftMargin: 16
        anchors.rightMargin: 16
        anchors.bottomMargin: 16

        GridView {
            id: grid
            // 网格模型：内置「在线下载」入口（首位，伪装成普通应用）+ 已装应用
            // （backend.apps 变化时整表重算）
            model: {
                const list = [{ storeEntry: true }]
                const apps = backend.apps || []
                for (let i = 0; i < apps.length; ++i) list.push(apps[i])
                return list
            }
            // 纯图标网格（不展示名称）：格子与高亮框都收窄为图标尺寸
            cellWidth: 92; cellHeight: 78
            clip: true
            topMargin: 58   // 通栏工具栏(44) + 间距 14

            delegate: Item {
                width: grid.cellWidth
                height: grid.cellHeight

                readonly property var app: modelData
                readonly property bool isStore: app.storeEntry === true

                // 悬停高亮：贴合格子的紧凑底框
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 3
                    radius: 12
                    color: appMouse.containsMouse ? Theme.cardHover : "transparent"
                }

                // 图标：设备真实图标（守护后台抓取缓存），未就绪回退字母块
                Rectangle {
                    anchors.centerIn: parent
                    width: 54; height: 54; radius: 13
                    color: isStore ? "transparent" : Theme.avatarColor(app.package)
                    clip: true

                    Image {
                        id: appIcon
                        anchors.fill: parent
                        anchors.margins: 4
                        source: !isStore && (app.icon && app.icon.length > 0)
                                ? "file://" + app.icon : ""
                        fillMode: Image.PreserveAspectCrop
                        smooth: true
                        visible: status === Image.Ready
                    }

                    // 「在线下载」入口图标：蓝色渐变 + 白色下载箭头（与真应用同规格）
                    Rectangle {
                        anchors.fill: parent
                        visible: isStore
                        radius: 13
                        gradient: Gradient {
                            GradientStop { position: 0; color: "#4EA5FF" }
                            GradientStop { position: 1; color: "#0A63E0" }
                        }
                        Item {
                            anchors.centerIn: parent
                            width: 26; height: 26
                            Image {
                                id: storeGlyph
                                anchors.fill: parent
                                source: "images/icons/download.svg"
                                fillMode: Image.PreserveAspectFit
                                sourceSize: Qt.size(52, 52)
                                visible: false
                            }
                            MultiEffect {
                                anchors.fill: storeGlyph
                                source: storeGlyph
                                colorization: 1.0
                                colorizationColor: "white"
                            }
                        }
                    }

                    Text {
                        anchors.centerIn: parent
                        visible: !isStore && !appIcon.visible
                        text: {
                            const n = app.name || app.package || "?"
                            return n.charAt(0).toUpperCase()
                        }
                        color: "white"
                        font.pixelSize: 22
                        font.weight: Font.DemiBold
                    }

                    // 运行中角标
                    Rectangle {
                        visible: app.running === true
                        anchors.top: parent.top; anchors.right: parent.right
                        anchors.margins: -3
                        width: 16; height: 16; radius: 8
                        color: Theme.success
                        border.color: Theme.card; border.width: 2
                    }

                    // 独立窗口徽标
                    Rectangle {
                        visible: app.mode === "window"
                        anchors.bottom: parent.bottom; anchors.left: parent.left
                        anchors.margins: -3
                        width: 18; height: 18; radius: 9
                        color: Theme.accent
                        border.color: Theme.card; border.width: 2
                        Text { anchors.centerIn: parent; text: "🪟"; font.pixelSize: 9 }
                    }
                }

                // 「在线下载」入口悬停提示（真应用保持纯图标无提示）
                HoverHandler { id: storeHover; enabled: isStore }
                ToolTip.visible: storeHover.hovered
                ToolTip.text: qsTr("在线下载 — 搜索并安装应用")
                ToolTip.delay: 350

                MouseArea {
                    id: appMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onClicked: (mouse) => {
                        if (isStore) {
                            page.openStore()
                            return
                        }
                        if (mouse.button === Qt.RightButton)
                            contextMenu.popup()
                    }
                    onDoubleClicked: {
                        if (!isStore) backend.launchApp(app.package)
                    }
                }

                Menu {
                    id: contextMenu
                    MenuItem {
                        text: qsTr("▶️ 启动")
                        onTriggered: backend.launchApp(app.package)
                    }
                    MenuSeparator {}
                    MenuItem {
                        text: app.mode === "window" ? qsTr("🖥 改为模拟器内打开")
                                                     : qsTr("🪟 设为独立窗口打开")
                        onTriggered: backend.setAppMode(
                            app.package,
                            app.mode === "window" ? "emulator" : "window")
                    }
                    MenuSeparator {}
                    MenuItem {
                        text: qsTr("🗑 卸载")
                        onTriggered: {
                            uninstallDialog.pkg = app.package
                            uninstallDialog.appName = app.name || app.package
                            uninstallDialog.open()
                        }
                    }
                }
            }
        }
    }

    // ===== 顶部工具栏（贴顶通栏毛玻璃，与窗口齐平）：运行/应用数 · CPU/内存 · 安装入口 =====
    Item {
        id: topBar
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 44

        // 毛玻璃底层：抓取壁纸在工具栏区域的画面并模糊（壁纸未就绪时自动隐藏）
        ShaderEffectSource {
            id: frostSource
            sourceItem: wallpaperBg
            sourceRect: Qt.rect(topBar.x, topBar.y, topBar.width, topBar.height)
            visible: false
        }
        MultiEffect {
            anchors.fill: parent
            source: frostSource
            visible: wallpaperBg.status === Image.Ready
            autoPaddingEnabled: false
            blur: 0.6
            blurMax: 32
        }

        // 半透明着色 + 底缘分隔线（浅深色自适应）
        Rectangle {
            anchors.fill: parent
            color: Theme.dark ? Qt.rgba(0.11, 0.11, 0.13, 0.50) : Qt.rgba(1, 1, 1, 0.55)
        }
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 1
            color: Theme.dark ? Qt.rgba(1, 1, 1, 0.08) : Qt.rgba(0, 0, 0, 0.06)
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 12
            anchors.rightMargin: 10
            spacing: 8

            StatPill {
                visible: backend.vmRunning
                dotColor: Theme.success
                value: page.runningApps
                tip: qsTr("%1 个应用正在运行").arg(page.runningApps)
            }
            StatPill {
                visible: backend.vmRunning
                icon: "images/icons/apps.svg"
                value: backend.appCount
                tip: qsTr("已装 %1 个应用 · 双击图标启动 · 右键更多操作")
                        .arg(backend.appCount)
            }

            Item { Layout.fillWidth: true }

            StatPill {
                visible: backend.vmRunning
                icon: "images/icons/cpu.svg"
                themedIcon: true
                value: page.cpuPct + "%"
                valueColor: page.cpuPct > 85 ? Theme.danger : Theme.text
                tip: qsTr("虚拟机 CPU 占用")
            }
            StatPill {
                visible: backend.vmRunning
                icon: "images/icons/ram.svg"
                themedIcon: true
                value: Math.round(page.memPct) + "%"
                valueColor: page.memPct > 85 ? Theme.danger : Theme.text
                tip: qsTr("虚拟机内存：已用 %1 / 共 %2")
                        .arg(page.fmtMB(page.memUsedMb)).arg(page.fmtMB(page.memTotalMb))
            }

            // 安装入口：下载图标圆钮；安装中仅转圈（无进度条）；失败 ✕。详情见悬停提示
            Rectangle {
                id: installBtn
                readonly property bool failed: !page.installBusy
                                               && page.installErr.length > 0
                Layout.preferredWidth: 30
                Layout.preferredHeight: 30
                Layout.alignment: Qt.AlignVCenter
                radius: width / 2
                color: installBtn.failed ? Theme.danger
                       : installMouse.pressed
                         ? (Theme.dark ? Qt.rgba(1, 1, 1, 0.16) : Qt.rgba(0, 0, 0, 0.10))
                         : installMouse.containsMouse
                           ? (Theme.dark ? Qt.rgba(1, 1, 1, 0.12) : Qt.rgba(0, 0, 0, 0.07))
                           : (Theme.dark ? Qt.rgba(1, 1, 1, 0.07) : Qt.rgba(0, 0, 0, 0.045))
                Behavior on color { ColorAnimation { duration: 100 } }

                Item {
                    anchors.centerIn: parent
                    width: 16; height: 16
                    visible: !page.installBusy && !installBtn.failed
                    Image {
                        id: installIcon
                        anchors.fill: parent
                        source: "images/icons/install.svg"
                        fillMode: Image.PreserveAspectFit
                        sourceSize: Qt.size(32, 32)
                        visible: false
                    }
                    MultiEffect {
                        anchors.fill: installIcon
                        source: installIcon
                        colorization: 1.0
                        colorizationColor: Theme.text
                    }
                }

                BusyIndicator {
                    anchors.centerIn: parent
                    visible: page.installBusy
                    running: visible
                    implicitWidth: 18; implicitHeight: 18
                }

                Text {
                    anchors.centerIn: parent
                    visible: installBtn.failed
                    text: "✕"
                    color: "white"
                    font.pixelSize: 13; font.weight: Font.Bold
                }

                MouseArea {
                    id: installMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: !backend.busy
                    cursorShape: Qt.PointingHandCursor
                    onClicked: apkPicker.open()
                }
                HoverHandler { id: installHover }
                ToolTip.visible: installHover.hovered
                ToolTip.delay: 250
                ToolTip.text: {
                    if (installBtn.failed)
                        return qsTr("安装失败：%1\n点击重新选择安装包").arg(page.installErr)
                    if (page.installBusy) {
                        const t = backend.installTask
                        const mb = (t.progress_total || 0) > 0
                                ? "  " + (t.progress / 1048576).toFixed(1) + " / "
                                  + (t.progress_total / 1048576).toFixed(1) + " MB" : ""
                        return (t.phase || qsTr("准备安装…")) + "  "
                               + Math.round(page.installFrac * 100) + "%" + mb
                    }
                    return qsTr("安装 APK（也可将 .apk 直接拖入窗口）")
                }
            }
        }
    }

    // guest 无响应提示（快照恢复后 adbd 失联等；守护健康看护会自动重启自愈）
    Rectangle {
        visible: backend.vmRunning && backend.appsError
        anchors.top: topBar.bottom
        anchors.topMargin: 10
        anchors.horizontalCenter: parent.horizontalCenter
        width: errPillText.implicitWidth + 38
        height: 32
        radius: 16
        color: Theme.dark ? Qt.rgba(0.26, 0.20, 0.05, 0.78) : Qt.rgba(1, 0.97, 0.89, 0.85)
        border.width: 1
        border.color: Theme.warnBorder

        Row {
            anchors.centerIn: parent
            spacing: 7
            Text { text: "⚠️"; font.pixelSize: 12; anchors.verticalCenter: parent.verticalCenter }
            Text {
                id: errPillText
                text: qsTr("Android 无响应，正在等待守护进程自动重启恢复…")
                font.pixelSize: 12
                color: Theme.warning
                anchors.verticalCenter: parent.verticalCenter
            }
        }
    }

    // ===== APK 安装进度浮层（拖放/选择安装包后的可视反馈）=====
    // 状态机：idle → running（传输→设备安装）→ success（2.6s 自动收起）/ error（手动关闭/重试）。
    // 只对「本会话观察到的 running→结束」迁移弹卡片，避免启动时把上次遗留任务当新完成。
    Rectangle {
        id: installCard
        property string state: "idle"          // idle | running | success | error
        property string wasRunning: ""         // "1" = 上一拍在跑（字符串存避免重绑）
        readonly property var task: backend.installTask
        readonly property string shownName: {
            const t = task
            const label = (t.label || "").length > 0 ? t.label : ""
            return label.length > 0 ? label : qsTr("安装包")
        }

        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottomMargin: 18
        width: 400
        height: 92
        radius: 16
        color: Theme.dark ? Qt.rgba(0.16, 0.16, 0.19, 0.96) : Qt.rgba(1, 1, 1, 0.97)
        border.width: 1
        border.color: state === "error" ? Theme.danger
                     : state === "success" ? Theme.success : Theme.separator
        opacity: state === "idle" ? 0 : 1
        scale: state === "idle" ? 0.94 : 1
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
        Behavior on scale { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }

        // 任务状态迁移监听（installTaskChanged 由 backend 1s 轮询驱动）
        Connections {
            target: backend
            function onInstallTaskChanged() {
                const t = installCard.task
                const running = t.running === true
                if (running) {
                    installCard.state = "running"
                    installCard.wasRunning = "1"
                    successHide.stop()
                } else if (installCard.wasRunning === "1") {
                    installCard.wasRunning = ""
                    installCard.state = ((t.error || "").length > 0) ? "error" : "success"
                    if (installCard.state === "success")
                        successHide.restart()
                }
            }
        }
        Timer { id: successHide; interval: 2600; onTriggered: installCard.state = "idle" }

        RowLayout {
            anchors.fill: parent
            anchors.margins: 14
            spacing: 12

            // 左：应用图标（daemon 从本地 APK 提取；缺失时脉冲占位），
            // running 时叠加环形进度；success 时 ✓ 绿圈
            Item {
                Layout.preferredWidth: 56
                Layout.preferredHeight: 56

                Rectangle {
                    anchors.fill: parent
                    radius: 13
                    color: Theme.avatarColor(installCard.task.package || "apk")
                    visible: !successRing.visible
                    Image {
                        anchors.fill: parent
                        anchors.margins: 4
                        source: (installCard.task.icon || "").length > 0
                                ? "file://" + installCard.task.icon : ""
                        fillMode: Image.PreserveAspectCrop
                        smooth: true
                        visible: status === Image.Ready
                    }
                    // 无图标脉冲占位（安卓小机器人字形兜底）
                    Text {
                        anchors.centerIn: parent
                        visible: !(installCard.task.icon || "").length
                        text: "🤖"
                        font.pixelSize: 26
                    }
                    SequentialAnimation on opacity {
                        running: installCard.state === "running" && !(installCard.task.icon || "").length
                        loops: Animation.Infinite
                        NumberAnimation { to: 0.45; duration: 700; easing.type: Easing.InOutQuad }
                        NumberAnimation { to: 1; duration: 700; easing.type: Easing.InOutQuad }
                    }
                }

                // 环形进度（传输阶段按字节；设备安装阶段扫转）
                Canvas {
                    id: ring
                    anchors.fill: parent
                    anchors.margins: -4
                    visible: installCard.state === "running"
                    readonly property double frac: {
                        const t = installCard.task
                        return (t.progress_total || 0) > 0
                                ? Math.min(1, (t.progress || 0) / t.progress_total) : 0
                    }
                    // 不定态扫转角度（设备安装中）
                    property double sweep: 0
                    NumberAnimation on sweep {
                        running: ring.visible && ring.frac <= 0
                        loops: Animation.Infinite
                        from: 0; to: 360; duration: 1100
                    }
                    onFracChanged: requestPaint()
                    onSweepChanged: requestPaint()
                    onPaint: {
                        const ctx = getContext("2d")
                        ctx.reset()
                        ctx.lineWidth = 3.5
                        ctx.strokeStyle = Theme.accent
                        ctx.beginPath()
                        if (frac > 0) {
                            ctx.arc(width / 2, height / 2, width / 2 - 2.5,
                                    -Math.PI / 2, -Math.PI / 2 + frac * Math.PI * 2)
                        } else {
                            const a = sweep * Math.PI / 180
                            ctx.arc(width / 2, height / 2, width / 2 - 2.5,
                                    a, a + Math.PI * 0.7)
                        }
                        ctx.stroke()
                    }
                }

                // 成功态：绿圈 ✓（弹入）
                Rectangle {
                    id: successRing
                    anchors.fill: parent
                    radius: height / 2
                    color: Theme.success
                    visible: installCard.state === "success"
                    scale: 1
                    Text { anchors.centerIn: parent; text: "✓"; color: "white"; font.pixelSize: 26; font.weight: Font.Bold }
                    SequentialAnimation on scale {
                        running: successRing.visible
                        NumberAnimation { to: 1.12; duration: 160; easing.type: Easing.OutQuad }
                        NumberAnimation { to: 1; duration: 200; easing.type: Easing.OutBack }
                    }
                }
            }

            // 中：名称 + 阶段文案 + 进度条
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 5

                Text {
                    text: installCard.state === "success"
                          ? qsTr("%1 已安装").arg(installCard.shownName)
                          : installCard.state === "error"
                            ? qsTr("安装 %1 失败").arg(installCard.shownName)
                            : installCard.shownName
                    font.pixelSize: 14; font.weight: Font.DemiBold
                    color: installCard.state === "error" ? Theme.danger : Theme.text
                    elide: Text.ElideMiddle
                    Layout.fillWidth: true
                }

                Text {
                    visible: installCard.state !== "error"
                    text: {
                        const t = installCard.task
                        if (installCard.state === "running") {
                            if ((t.progress_total || 0) > 0) {
                                return qsTr("传输 APK %1% · %2 / %3 MB")
                                        .arg(Math.round((t.progress || 0) * 100 / t.progress_total))
                                        .arg(((t.progress || 0) / 1048576).toFixed(1))
                                        .arg((t.progress_total / 1048576).toFixed(1))
                            }
                            return qsTr("设备安装中（dexopt 视包大小约 10–60s）…")
                        }
                        return qsTr("安装完成，图标即将出现在应用库")
                    }
                    font.pixelSize: 12
                    color: Theme.textDim
                    elide: Text.ElideMiddle
                    Layout.fillWidth: true
                }

                Text {
                    visible: installCard.state === "error"
                    text: installCard.task.error || ""
                    font.pixelSize: 12
                    color: Theme.danger
                    elide: Text.ElideMiddle
                    Layout.fillWidth: true
                }

                // 细进度条（传输定长 / 设备安装扫描）
                Rectangle {
                    visible: installCard.state === "running"
                    Layout.fillWidth: true
                    height: 5
                    radius: 2.5
                    color: Theme.field
                    clip: true

                    Rectangle {
                        id: barFill
                        property double frac: (installCard.task.progress_total || 0) > 0
                                ? Math.min(1, (installCard.task.progress || 0) / installCard.task.progress_total) : 0
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        height: parent.height
                        radius: 2.5
                        color: Theme.accent
                        width: frac > 0 ? parent.width * frac : 0
                        Behavior on width { NumberAnimation { duration: 200; easing.type: Easing.OutCubic } }

                        // 不定态滑块（设备安装中）
                        Rectangle {
                            visible: barFill.frac <= 0 && installCard.state === "running"
                            width: 56; height: parent.height; radius: 2.5
                            color: Theme.accent
                            SequentialAnimation on x {
                                running: visible
                                loops: Animation.Infinite
                                NumberAnimation { to: 340; duration: 900; easing.type: Easing.InOutQuad }
                                NumberAnimation { to: 0; duration: 900; easing.type: Easing.InOutQuad }
                            }
                        }
                    }
                }
            }

            // 右：错误态操作（重试 / 关闭）；运行态显示转圈
            ColumnLayout {
                visible: installCard.state === "error" || installCard.state === "running"
                spacing: 6
                BusyIndicator {
                    visible: installCard.state === "running"
                    running: visible
                    implicitWidth: 22; implicitHeight: 22
                }
                RButton {
                    visible: installCard.state === "error"
                    text: qsTr("重试")
                    onClicked: backend.retryInstall()
                }
            }

            // 关闭按钮（成功/失败态可手动收起）
            Text {
                visible: installCard.state === "success" || installCard.state === "error"
                text: "✕"
                font.pixelSize: 12
                color: Theme.textDim
                Layout.alignment: Qt.AlignTop | Qt.AlignRight
                MouseArea {
                    anchors.fill: parent
                    anchors.margins: -8
                    cursorShape: Qt.PointingHandCursor
                    onClicked: installCard.state = "idle"
                }
            }
        }
    }

    // 未运行：居中轻量引导（无边框卡片，保持页面简洁）
    ColumnLayout {
        visible: !backend.vmRunning
        anchors.centerIn: parent
        spacing: 10

        Text { text: "💤"; font.pixelSize: 40; Layout.alignment: Qt.AlignHCenter }
        Text {
            text: qsTr("Android 未运行")
            font.pixelSize: 15; font.weight: Font.DemiBold; color: Theme.text
            Layout.alignment: Qt.AlignHCenter
        }
        Text {
            text: qsTr("启动后即可查看与安装应用；也可直接把 .apk 拖进窗口")
            font.pixelSize: 12; color: Theme.textDim
            Layout.alignment: Qt.AlignHCenter
        }
        RButton {
            Layout.topMargin: 8
            text: qsTr("启动 Android")
            highlighted: true
            onClicked: backend.startVm()
            Layout.alignment: Qt.AlignHCenter
        }
    }

    // 卸载确认
    Dialog {
        id: uninstallDialog
        property string pkg: ""
        property string appName: ""
        modal: true
        anchors.centerIn: parent
        title: qsTr("卸载应用")
        standardButtons: Dialog.Cancel | Dialog.Yes
        width: 360

        Label {
            width: parent ? parent.width : 300
            wrapMode: Text.WordWrap
            color: Theme.text
            text: qsTr("将卸载 ") + (uninstallDialog.appName || uninstallDialog.pkg) +
                  qsTr("，其应用数据将一并删除。")
        }
        onAccepted: backend.uninstallApp(pkg)
    }

    FileDialog {
        id: apkPicker
        title: qsTr("选择 APK 安装包")
        nameFilters: [qsTr("Android 安装包") + " (*.apk *.apks *.xapk)"]
        onAccepted: backend.installApk(selectedFile.toString(), false)
    }
}
