import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import Houttuynia

Item {
    id: page

    // ===== 性能数据（原独立性能页并入概览）=====
    readonly property int cpuPct: Math.round(backend.perf.cpu_pct || 0)
    readonly property double memTotalMb: backend.perf.mem ? (backend.perf.mem.total_mb || 0) : 0
    readonly property double memAvailMb: backend.perf.mem ? (backend.perf.mem.available_mb || 0) : 0
    readonly property double memUsedMb: backend.perf.mem ? Math.max(0, memTotalMb - memAvailMb) : 0
    readonly property double memPct: memTotalMb > 0 ? memUsedMb / memTotalMb * 100 : 0
    readonly property double hostCpuPct: backend.perf.host ? (backend.perf.host.qemu_cpu_pct || 0) : 0
    readonly property double hostMemMb: backend.perf.host ? (backend.perf.host.qemu_mem_mb || 0) : 0

    // 内存容量展示：≥1 GB 用 GB（一位小数，整数值不带 .0），不足 1 GB 用 MB
    function fmtMB(mb) {
        if (mb >= 1024) {
            const gb = (mb / 1024).toFixed(1)
            return (gb.endsWith(".0") ? gb.slice(0, -2) : gb) + " GB"
        }
        return Math.round(mb) + " MB"
    }

    function fmtUptime(s) {
        if (!s) return "—"
        const h = Math.floor(s / 3600), m = Math.floor((s % 3600) / 60)
        return h > 0 ? h + qsTr(" 小时 ") + m + qsTr(" 分") : m + qsTr(" 分 ") + (s % 60) + qsTr(" 秒")
    }

    // ===== 运行时首启引导 =====
    // 装完应用但运行时缺失时主动提示一次（在线下载 / 导入分发包），本次会话只弹一次。
    // runtimeKnown 门控：启动早期 runtime 状态还没查回来（默认缺失），不弹
    // ——否则本机已装好环境的用户也会被误弹
    property bool runtimePrompted: false
    function maybePromptRuntime() {
        if (runtimePrompted || !backend.daemonOnline || !backend.runtimeKnown
                || backend.runtimePresent || backend.sdkAvailable
                || backend.runtimeTask.running)
            return
        runtimePrompted = true
        runtimeSetupDialog.open()
    }
    Component.onCompleted: maybePromptRuntime()
    Connections {
        target: backend
        function onRuntimeChanged() { maybePromptRuntime() }
        function onStatusChanged() { maybePromptRuntime() }
    }

    // 运行时任务态快捷取值
    readonly property var rtTask: backend.runtimeTask
    readonly property bool rtRunning: rtTask.running === true
    readonly property string rtError: rtTask.error || ""
    readonly property bool rtDownloadPhase: (rtTask.phase || "").startsWith(qsTr("下载"))
    readonly property double rtFrac: (rtTask.progress_total || 0) > 0
            ? Math.min(1, (rtTask.progress || 0) / rtTask.progress_total) : 0

    // 内容并入性能区块后变长，小窗口下改为纵向滚动
    ScrollView {
        anchors.fill: parent
        contentWidth: availableWidth

        ColumnLayout {
            x: 24; y: 20
            width: parent.width - 48
            spacing: 14

            RowLayout {
                Layout.fillWidth: true
                Text {
                    text: qsTr("概览")
                    font.pixelSize: 22; font.weight: Font.Bold
                    color: Theme.text
                }
                Item { Layout.fillWidth: true }
                RButton {
                    text: qsTr("📷 截屏")
                    enabled: backend.vmRunning
                    onClicked: backend.screenshot()
                }
            }

            // 运行时缺失卡片：在线下载为主（一键断点续传），本地导入为辅
            Rectangle {
                visible: backend.daemonOnline && !backend.runtimePresent && !page.rtRunning
                Layout.fillWidth: true
                height: 108
                radius: Theme.radius
                color: Theme.warnBg
                border.color: Theme.warnBorder; border.width: 1

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 16
                    spacing: 14

                    Rectangle {
                        Layout.preferredWidth: 52
                        Layout.preferredHeight: 52
                        radius: 14
                        color: Theme.dark ? Qt.rgba(1, 1, 1, 0.08) : Qt.rgba(0, 0, 0, 0.05)
                        Text { anchors.centerIn: parent; text: "⚙️"; font.pixelSize: 26 }
                    }

                    ColumnLayout {
                        spacing: 3
                        Layout.fillWidth: true
                        Text {
                            text: qsTr("模拟器运行时未安装")
                            font.pixelSize: 15; font.weight: Font.DemiBold
                            color: Theme.warning
                        }
                        Text {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: backend.sdkAvailable
                                  ? qsTr("检测到本机 Android SDK，可直接启动；也可在线下载 Houttuynia 运行时（约 330 MB，一次安装永久保留）")
                                  : qsTr("运行时（模拟器核心）不随安装包分发。在线下载约 330 MB（支持断点续传），一次安装永久保留；或导入本地分发包")
                            font.pixelSize: 12
                            color: Theme.warning
                            opacity: 0.9
                        }
                        // 上次失败原因（任务静默后仍显示，重试入口在按钮行）
                        Text {
                            visible: page.rtError.length > 0
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: qsTr("上次失败：") + page.rtError
                            font.pixelSize: 11
                            color: Theme.danger
                        }
                    }

                    ColumnLayout {
                        spacing: 8
                        RButton {
                            Layout.alignment: Qt.AlignRight
                            text: qsTr("⬇ 在线下载")
                            highlighted: true
                            onClicked: backend.runtimeDownload()
                        }
                        RButton {
                            Layout.alignment: Qt.AlignRight
                            text: qsTr("导入本地分发包…")
                            onClicked: runtimePicker.open()
                        }
                    }
                }
            }

            // 运行时任务进度（下载：百分比 + 速度 + 取消；解压：不定态）
            Rectangle {
                visible: page.rtRunning
                Layout.fillWidth: true
                height: 96
                radius: Theme.radius
                color: Theme.card
                border.color: Theme.separator

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 6

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10
                        BusyIndicator {
                            implicitWidth: 20; implicitHeight: 20
                            running: visible
                        }
                        Text {
                            Layout.fillWidth: true
                            text: (page.rtTask.phase || qsTr("进行中…"))
                                  + (page.rtDownloadPhase && (page.rtTask.progress_total || 0) > 0
                                     ? "  ·  " + page.fmtMB(page.rtTask.progress / 1048576)
                                       + " / " + page.fmtMB(page.rtTask.progress_total / 1048576)
                                     : "")
                            font.pixelSize: 13
                            color: Theme.text
                            elide: Text.ElideMiddle
                        }
                        Text {
                            visible: page.rtDownloadPhase && (page.rtTask.progress_total || 0) > 0
                            text: Math.round(page.rtFrac * 100) + "%"
                            font.pixelSize: 20; font.weight: Font.Bold
                            color: Theme.accent
                        }
                        RButton {
                            visible: page.rtDownloadPhase
                            text: qsTr("取消")
                            onClicked: backend.runtimeCancel()
                        }
                    }

                    // 进度条：下载定长推进；解压/导入不定态滑块
                    Rectangle {
                        Layout.fillWidth: true
                        height: 6
                        radius: 3
                        color: Theme.field
                        clip: true

                        Rectangle {
                            visible: page.rtDownloadPhase && (page.rtTask.progress_total || 0) > 0
                            width: parent.width * page.rtFrac
                            height: parent.height
                            radius: 3
                            color: Theme.accent
                            Behavior on width { NumberAnimation { duration: 300; easing.type: Easing.OutCubic } }
                        }
                        Rectangle {
                            visible: !(page.rtDownloadPhase && (page.rtTask.progress_total || 0) > 0)
                            width: 64; height: parent.height; radius: 3
                            color: Theme.accent
                            SequentialAnimation on x {
                                running: visible
                                loops: Animation.Infinite
                                NumberAnimation { to: 420; duration: 1000; easing.type: Easing.InOutQuad }
                                NumberAnimation { to: 0; duration: 1000; easing.type: Easing.InOutQuad }
                            }
                        }
                    }

                    Text {
                        visible: page.rtDownloadPhase
                        text: qsTr("下载中断会保留进度，重新点击在线下载即续传")
                        font.pixelSize: 11
                        color: Theme.textDim
                    }
                }
            }

            // VM 状态卡片
            Rectangle {
                Layout.fillWidth: true
                height: 130
                radius: Theme.radius
                color: Theme.card
                border.color: Theme.separator

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 20
                    spacing: 16

                    Rectangle {
                        width: 56; height: 56; radius: 28
                        color: backend.vmRunning ? "#DFF5E1" : (Theme.dark ? "#333338" : "#F0F0F2")
                        Text {
                            anchors.centerIn: parent
                            text: backend.vmRunning ? "🤖" : (backend.vmBooting ? "⏳" : "💤")
                            font.pixelSize: 26
                        }
                    }

                    ColumnLayout {
                        spacing: 4
                        Text {
                            text: backend.vmBooting && !backend.vmRunning
                                  ? (backend.runtimePresent ? qsTr("冷启动中（首次约 15–30 秒）")
                                                            : qsTr("启动中（需先安装运行时）"))
                                  : backend.vmRunning ? qsTr("Android 运行中") : qsTr("虚拟机已关机")
                            font.pixelSize: 18; font.weight: Font.DemiBold
                            color: Theme.text
                        }
                        Text {
                            text: backend.vmRunning
                                  ? qsTr("设备 ") + backend.vmSerial + qsTr(" · 快照关机后 5 秒级恢复现场")
                                  : backend.vmError && backend.daemonOnline
                                    ? qsTr("上次错误：") + backend.vmError
                                    : qsTr("点击启动开始使用 Android 应用（关机自动存档）")
                            font.pixelSize: 13
                            color: Theme.textDim
                            elide: Text.ElideMiddle
                            Layout.maximumWidth: 420
                        }
                    }

                    Item { Layout.fillWidth: true }

                    BusyIndicator {
                        visible: backend.vmBooting && !backend.vmRunning
                        running: visible
                        implicitWidth: 30; implicitHeight: 30
                    }
                    RButton {
                        text: backend.vmRunning ? qsTr("重启") : qsTr("启动")
                        highlighted: true
                        onClicked: {
                            if (backend.vmRunning) {
                                backend.stopVm()
                                restartDelay.restart()
                            } else {
                                backend.startVm()
                            }
                        }
                        Timer {
                            id: restartDelay
                            interval: 6000   // 等关停落定再拉起
                            onTriggered: backend.startVm()
                        }
                    }
                    RButton {
                        text: qsTr("🖥 显示")
                        visible: backend.vmRunning
                        onClicked: backend.showVmWindow()
                        ToolTip.visible: displayHover.hovered
                        ToolTip.delay: 400
                        ToolTip.text: qsTr("显示 Android 主屏 emulator 窗口（独立窗口模式下会快照重启切换，约 5-10 秒）")
                        HoverHandler { id: displayHover }
                    }
                    RButton {
                        text: qsTr("存档关机")
                        visible: backend.vmRunning
                        onClicked: backend.stopVm()
                    }
                }
            }

            // 快速操作
            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                Repeater {
                    model: [
                        { icon: "📱", label: qsTr("打开应用库"), page: 1 },
                        { icon: "⬇️", label: qsTr("下载应用"), store: true },
                        { icon: "⌨️", label: qsTr("ADB 终端"), page: 3 },
                    ]
                    delegate: Rectangle {
                        Layout.fillWidth: true
                        height: 84
                        radius: Theme.radius
                        color: quickMouse.containsMouse ? Theme.cardHover : Theme.card
                        border.color: Theme.separator

                        ColumnLayout {
                            anchors.centerIn: parent
                            spacing: 4
                            Text { text: modelData.icon; font.pixelSize: 22; Layout.alignment: Qt.AlignHCenter }
                            Text {
                                text: modelData.label; font.pixelSize: 12
                                color: Theme.textDim
                                Layout.alignment: Qt.AlignHCenter
                            }
                        }
                        MouseArea {
                            id: quickMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                let n = page
                                while (n && !(n instanceof ApplicationWindow)) n = n.parent
                                if (!n) return
                                // 下载应用：弹「在线下载」独立窗口（非页面跳转）
                                if (modelData.store) n.openStoreWindow()
                                else n.currentPage = modelData.page
                            }
                        }
                    }
                }
            }

            // ===== 性能监控（原独立页并入；VM 运行时显示）=====
            Text {
                visible: backend.vmRunning
                text: qsTr("性能监控")
                font.pixelSize: 15; font.weight: Font.DemiBold
                color: Theme.text
            }

            RowLayout {
                visible: backend.vmRunning
                Layout.fillWidth: true
                spacing: 12

                // guest CPU
                Rectangle {
                    Layout.fillWidth: true
                    height: 104
                    radius: Theme.radius
                    color: Theme.card
                    border.color: Theme.separator

                    ColumnLayout {
                        anchors.centerIn: parent
                        spacing: 2
                        Text {
                            text: page.cpuPct + "%"
                            font.pixelSize: 27; font.weight: Font.Bold
                            color: page.cpuPct > 85 ? Theme.danger : Theme.text
                        }
                        Text { text: "guest CPU"; font.pixelSize: 11; color: Theme.textDim }
                    }
                }

                // guest 内存：容量自适应 MB/GB + 使用率条（按占用阈值变色）
                Rectangle {
                    Layout.fillWidth: true
                    height: 104
                    radius: Theme.radius
                    color: Theme.card
                    border.color: Theme.separator

                    ColumnLayout {
                        anchors.centerIn: parent
                        spacing: 5

                        Text {
                            text: page.fmtMB(page.memUsedMb) + " / " + page.fmtMB(page.memTotalMb)
                            font.pixelSize: 20; font.weight: Font.Bold
                            color: page.memPct > 90 ? Theme.danger : Theme.text
                        }
                        Rectangle {
                            Layout.preferredWidth: 150
                            height: 6; radius: 3
                            color: Theme.field
                            border.color: Theme.separator; border.width: 1
                            Rectangle {
                                width: Math.min(parent.width, parent.width * page.memPct / 100)
                                height: parent.height; radius: 3
                                color: page.memPct > 90 ? Theme.danger
                                     : page.memPct > 75 ? Theme.warning : Theme.accent
                                Behavior on width { NumberAnimation { duration: 300; easing.type: Easing.OutCubic } }
                            }
                        }
                        Text {
                            text: qsTr("guest 内存 · 已用 %1% · 可用 %2")
                                      .arg(Math.round(page.memPct))
                                      .arg(page.fmtMB(page.memAvailMb))
                            font.pixelSize: 11; color: Theme.textDim
                        }
                    }
                }

                // 运行时长 + 宿主开销
                Rectangle {
                    Layout.fillWidth: true
                    height: 104
                    radius: Theme.radius
                    color: Theme.card
                    border.color: Theme.separator

                    ColumnLayout {
                        anchors.centerIn: parent
                        spacing: 3
                        Text {
                            text: page.fmtUptime(backend.perf.uptime_s || 0)
                            font.pixelSize: 16; font.weight: Font.DemiBold; color: Theme.text
                        }
                        Text { text: qsTr("运行时长"); font.pixelSize: 11; color: Theme.textDim }
                        Rectangle { width: 60; height: 1; color: Theme.separator }
                        Text {
                            text: qsTr("宿主 qemu：%1% CPU · %2 内存")
                                      .arg(Math.round(page.hostCpuPct))
                                      .arg(page.fmtMB(page.hostMemMb))
                            font.pixelSize: 12; color: Theme.textDim
                        }
                    }
                }
            }

            // 3 分钟历史曲线
            Rectangle {
                visible: backend.vmRunning
                Layout.fillWidth: true
                height: 148
                radius: Theme.radius
                color: Theme.card
                border.color: Theme.separator

                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 6

                    RowLayout {
                        Layout.fillWidth: true
                        Text {
                            text: qsTr("3 分钟历史")
                            font.pixelSize: 13; font.weight: Font.DemiBold; color: Theme.text
                        }
                        Item { Layout.fillWidth: true }
                        RowLayout {
                            spacing: 4
                            Rectangle { width: 10; height: 3; radius: 1; color: Theme.accent }
                            Text { text: "CPU %"; font.pixelSize: 11; color: Theme.textDim }
                        }
                        RowLayout {
                            spacing: 4
                            Rectangle { width: 10; height: 3; radius: 1; color: Theme.success }
                            Text { text: qsTr("内存 %"); font.pixelSize: 11; color: Theme.textDim }
                        }
                    }

                    Canvas {
                        id: chart
                        Layout.fillWidth: true
                        Layout.fillHeight: true

                        onPaint: {
                            const ctx = getContext("2d")
                            ctx.reset()
                            const w = width, h = height
                            const hist = backend.perfHistory

                            // 网格
                            ctx.strokeStyle = Theme.dark ? "#3A3A3F" : "#EBEBEE"
                            ctx.lineWidth = 1
                            for (let g = 0; g <= 4; g++) {
                                const y = h * g / 4
                                ctx.beginPath()
                                ctx.moveTo(0, y); ctx.lineTo(w, y)
                                ctx.stroke()
                            }

                            const drawSeries = function(getter, color) {
                                if (hist.length < 2) return
                                ctx.strokeStyle = color
                                ctx.lineWidth = 1.6
                                ctx.beginPath()
                                for (let i = 0; i < hist.length; i++) {
                                    const x = i / (90 - 1) * w   // 固定 90 点窗口（3 分钟）
                                    const v = Math.min(100, Math.max(0, getter(hist[i])))
                                    const y = h - (v / 100) * h
                                    i === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y)
                                }
                                ctx.stroke()
                            }
                            drawSeries(function(s) { return s.cpu }, Theme.accent)
                            drawSeries(function(s) { return s.memPct }, Theme.success)
                        }

                        Connections {
                            target: backend
                            function onPerfChanged() { chart.requestPaint() }
                        }
                        onWidthChanged: requestPaint()
                        onHeightChanged: requestPaint()
                    }

                    Text {
                        text: qsTr("← 3 分钟前 · 2 秒采样 · 当前 →")
                        font.pixelSize: 10; color: Theme.textFaint
                        Layout.alignment: Qt.AlignHCenter
                    }
                }
            }

            // 快照速览
            Rectangle {
                visible: backend.snapshots.length > 0
                Layout.fillWidth: true
                height: 52
                radius: Theme.radius
                color: Theme.card
                border.color: Theme.separator

                RowLayout {
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 10
                    Text { text: "💾"; font.pixelSize: 16 }
                    Text {
                        text: qsTr("存档点：") + backend.snapshots.length + qsTr(" 个（设置页可管理）")
                        font.pixelSize: 13; color: Theme.textDim
                    }
                    Item { Layout.fillWidth: true }
                }
            }

            Text {
                Layout.topMargin: 2
                Layout.bottomMargin: 6
                text: qsTr("跨平台 Qt 前端 · 守护进程 REST API · 独立窗口 Qt/OpenGL 渲染 + VideoToolbox 硬解")
                font.pixelSize: 11
                color: Theme.textFaint
            }
        }
    }

    FileDialog {
        id: runtimePicker
        title: qsTr("选择运行时分发包")
        nameFilters: [qsTr("运行时包") + " (*.zip)"]
        onAccepted: backend.importRuntime(selectedFile.toString())
    }

    // 首启运行时引导弹窗（daemon 在线且无运行时/SDK 时自动弹一次）
    Dialog {
        id: runtimeSetupDialog
        modal: true
        anchors.centerIn: parent
        width: 420
        padding: 0
        title: qsTr("欢迎使用 Houttuynia")

        background: Rectangle {
            radius: 16
            color: Theme.card
            border.color: Theme.separator
        }

        contentItem: ColumnLayout {
            spacing: 10

            Rectangle {
                Layout.alignment: Qt.AlignHCenter
                Layout.topMargin: 20
                width: 64; height: 64; radius: 16
                Image {
                    anchors.fill: parent
                    source: "images/logo-appicon.png"
                    fillMode: Image.PreserveAspectFit
                    mipmap: true
                }
            }
            Text {
                Layout.alignment: Qt.AlignHCenter
                text: qsTr("还差一步：下载模拟器运行时")
                font.pixelSize: 16; font.weight: Font.DemiBold
                color: Theme.text
            }
            Text {
                Layout.fillWidth: true
                Layout.leftMargin: 24; Layout.rightMargin: 24
                wrapMode: Text.WordWrap
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("运行时（Android 模拟器核心，约 330 MB）不随安装包分发，需在线下载或导入本地分发包。一次安装永久保留，系统镜像稍后在「镜像」页拉取。")
                font.pixelSize: 12
                color: Theme.textDim
            }
            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                Layout.bottomMargin: 20
                spacing: 10
                RButton {
                    text: qsTr("⬇ 在线下载")
                    highlighted: true
                    onClicked: {
                        runtimeSetupDialog.close()
                        backend.runtimeDownload()
                    }
                }
                RButton {
                    text: qsTr("导入本地包…")
                    onClicked: {
                        runtimeSetupDialog.close()
                        runtimePicker.open()
                    }
                }
                RButton {
                    text: qsTr("以后再说")
                    onClicked: runtimeSetupDialog.close()
                }
            }
        }
    }
}
