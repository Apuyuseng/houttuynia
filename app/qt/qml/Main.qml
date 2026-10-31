import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import QtQuick.Effects
import Houttuynia

ApplicationWindow {
    id: root
    visible: true
    x: 120; y: 100
    width: 1060; height: 720
    minimumWidth: 860; minimumHeight: 560
    title: "Houttuynia"
    color: Theme.window

    property int currentPage: 0
    property string selectedKeysPkg: ""   // 跨页跳转：按键映射页预选包名

    // 页面索引（与侧栏 / StackLayout 对应）；icon 为彩色线条 SVG
    // （images/icons/<name>.svg，每个菜单一种颜色），激活时由 MultiEffect 漂白
    // 在线下载不是页面：它是应用库网格里的内置应用，经 openStoreWindow() 弹独立窗口
    // 按键映射暂未开放（技术不成熟）——入口隐藏，KeysPage.qml 保留待后续启用
    // 性能监控已并入概览页（VM 运行时随性能区块展示）
    // 关于 / 许可是页面（LGPL 合规入口），不再是弹框
    // 流量分析默认关闭（设置页开关）；gated 条目仅在开关开启时显示，索引恒定
    readonly property var pages: [
        { icon: "overview", label: qsTr("概览") },
        { icon: "apps", label: qsTr("应用库") },
        { icon: "devices", label: qsTr("真机") },
        { icon: "images", label: qsTr("镜像") },
        { icon: "terminal", label: qsTr("终端") },
        { icon: "capture", label: qsTr("流量分析"), gated: true },
        { icon: "settings", label: qsTr("设置") },
        { icon: "info", label: qsTr("关于 / 许可") },
    ]

    // 打开「在线下载」独立窗口（已开则前置；关闭仅隐藏，保留搜索/任务状态）
    function openStoreWindow() {
        if (storeLoader.status === Loader.Ready && storeLoader.item) {
            storeLoader.item.show()
            storeLoader.item.raise()
            storeLoader.item.requestActivate()
        } else {
            storeLoader.active = true
        }
    }
    Loader {
        id: storeLoader
        active: false
        source: "StoreWindow.qml"
        onLoaded: {
            item.show()
            item.raise()
            item.requestActivate()
        }
    }

    function showToast(msg, type) {
        toastText.text = msg
        toastBar.color = type === "error" ? Theme.danger
                       : type === "warn"  ? Theme.warning : Theme.success
        toastBar.visible = true
        toastTimer.restart()
    }
    Timer { id: toastTimer; interval: 3000; onTriggered: toastBar.visible = false }

    Connections {
        target: backend
        function onToast(message, type) { root.showToast(message, type) }
        function onConfirmationRequested(action, message) {
            confirmDialog.action = action
            confirmText.text = message
            confirmDialog.open()
        }
        // 流量分析开关关闭：正停留在该页则跳回概览（条目随绑定隐藏）
        function onCaptureEnabledChanged() {
            if (!backend.captureEnabled && root.currentPage === 5)
                root.currentPage = 0
        }
    }

    // 页面级轮询开关（性能/流量仅在可见时轮询，避免空转 adb）
    function activatePagePolling() {
        // 性能数据消费方：概览性能区块(0)、应用库顶栏(1)；流量分析页(5)
        backend.setPerfActive(currentPage === 0 || currentPage === 1)
        backend.setCaptureActive(currentPage === 5)
    }
    onCurrentPageChanged: activatePagePolling()
    Component.onCompleted: {
        // UI 回归钩子：初始页可由环境注入（typeof 兼容未注入时）
        if (typeof initialPage !== "undefined" && initialPage >= 0)
            currentPage = initialPage
        activatePagePolling()
        // UI 回归钩子：MWINE_OPEN_STORE=1 时启动即弹「在线下载」独立窗口
        if (typeof openStoreOnStart !== "undefined" && openStoreOnStart)
            openStoreWindow()
        // UI 回归钩子：MWINE_OPEN_ABOUT=1 时启动即切到「关于 / 许可」页
        if (typeof openAboutOnStart !== "undefined" && openAboutOnStart)
            currentPage = pages.length - 1
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // ===== 侧栏 =====
        Rectangle {
            Layout.preferredWidth: 200
            Layout.fillHeight: true
            color: Theme.sidebar

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 2

                RowLayout {
                    Layout.leftMargin: 8
                    Layout.bottomMargin: 14
                    spacing: 8
                    Image {
                        source: "images/logo-appicon.png"
                        // Layout 管理的子项必须用 Layout.preferred*，直接
                        // width/height 会被忽略（按 implicitSize 128 参与布局）
                        Layout.preferredWidth: 26
                        Layout.preferredHeight: 26
                        fillMode: Image.PreserveAspectFit
                        mipmap: true
                    }
                    Text {
                        text: "Houttuynia"
                        font.pixelSize: 16; font.weight: Font.DemiBold
                        color: Theme.text
                    }
                }

                Repeater {
                    model: root.pages
                    delegate: Rectangle {
                        readonly property bool active: root.currentPage === index
                        // gated 条目（流量分析）：开关未开启时不显示
                        //（ColumnLayout 跳过 invisible 项，不占位；索引恒定）
                        visible: !modelData.gated || backend.captureEnabled
                        Layout.fillWidth: true
                        height: 36
                        radius: 8
                        color: active ? Theme.accent
                             : (navMouse.containsMouse ? Theme.cardHover : "transparent")

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: 8
                            spacing: 8

                            // 导航图标：SVG 自带彩色，直接显示；
                            // 仅激活项经 MultiEffect 漂白为白（衬在 accent 底色上）
                            Item {
                                Layout.preferredWidth: 17
                                Layout.preferredHeight: 17
                                Image {
                                    id: navIcon
                                    anchors.fill: parent
                                    source: "images/icons/" + modelData.icon + ".svg"
                                    fillMode: Image.PreserveAspectFit
                                    sourceSize: Qt.size(34, 34)
                                    visible: false
                                }
                                MultiEffect {
                                    anchors.fill: navIcon
                                    source: navIcon
                                    colorizationColor: "white"
                                    colorization: active ? 1.0 : 0.0
                                }
                            }
                            Text {
                                text: modelData.label
                                font.pixelSize: 14
                                font.weight: active ? Font.DemiBold : Font.Normal
                                color: active ? "white" : Theme.text
                            }
                            Item { Layout.fillWidth: true }
                        }

                        MouseArea {
                            id: navMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.currentPage = index
                        }
                    }
                }

                // 关于已并入导航（pages 末项，LGPL 合规入口），不再单独占位

                Item { Layout.fillHeight: true }

                // VM 状态指示
                RowLayout {
                    Layout.leftMargin: 8
                    Layout.topMargin: 8
                    spacing: 6
                    Rectangle {
                        width: 8; height: 8; radius: 4
                        color: !backend.daemonOnline ? Theme.warning
                             : backend.vmRunning ? Theme.success : Theme.textFaint
                    }
                    Text {
                        text: !backend.daemonOnline ? qsTr("连接守护进程…")
                             : backend.vmBooting ? qsTr("启动中…")
                             : backend.vmRunning ? qsTr("Android 运行中") : qsTr("已关机")
                        font.pixelSize: 11
                        color: Theme.textDim
                    }
                }
            }
        }

        // ===== 内容区 =====
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            color: Theme.window

            StackLayout {
                anchors.fill: parent
                currentIndex: root.currentPage

                OverviewPage {}
                AppsPage {}
                DevicesPage {}
                ImagesPage {}
                TerminalPage {}
                CapturePage {}
                SettingsPage {}
                AboutPage {}
            }

            // 守护进程离线横幅（自动拉起中）
            Rectangle {
                visible: !backend.daemonOnline
                anchors.top: parent.top
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.topMargin: 16
                width: offlineText.implicitWidth + 40
                height: 38
                radius: Theme.radius
                color: Theme.warnBg
                border.color: Theme.warnBorder; border.width: 1
                Text {
                    id: offlineText
                    anchors.centerIn: parent
                    text: qsTr("守护进程未运行，正在自动启动 mwine-cored…")
                    color: Theme.warning
                    font.pixelSize: 13
                }
            }
        }
    }

    // ===== 全窗拖放 APK 安装 =====
    DropArea {
        id: apkDrop
        anchors.fill: parent
        // 只对 APK 类文件亮提示/接受，避免普通文件拖过窗口时误亮
        property bool hasApk: false
        onEntered: (drag) => {
            hasApk = false
            for (const url of drag.urls) {
                const s = String(url).toLowerCase()
                if (s.endsWith(".apk") || s.endsWith(".apks") || s.endsWith(".xapk")) {
                    hasApk = true
                    break
                }
            }
            if (hasApk)
                drag.accept(Qt.CopyAction)
        }
        onDropped: (drop) => {
            for (const url of drop.urls) {
                const s = String(url).toLowerCase()
                if (s.endsWith(".apk") || s.endsWith(".apks") || s.endsWith(".xapk")) {
                    backend.installApk(url, false)
                }
            }
        }
    }

    // 拖放悬浮提示：半透明压暗 + 中央引导卡（松开即安装）
    Rectangle {
        visible: apkDrop.containsDrag && apkDrop.hasApk
        anchors.fill: parent
        color: Qt.rgba(0, 0, 0, 0.38)
        z: 100

        Rectangle {
            anchors.centerIn: parent
            width: 300
            height: 150
            radius: 18
            color: Theme.dark ? "#2A2A30" : "#FFFFFF"
            border.color: Theme.accent
            border.width: 2

            ColumnLayout {
                anchors.centerIn: parent
                spacing: 8

                Rectangle {
                    Layout.alignment: Qt.AlignHCenter
                    width: 52; height: 52; radius: 26
                    color: Theme.accent
                    // 轻微呼吸（下载感）
                    SequentialAnimation on scale {
                        loops: Animation.Infinite
                        NumberAnimation { to: 1.1; duration: 650; easing.type: Easing.InOutQuad }
                        NumberAnimation { to: 1.0; duration: 650; easing.type: Easing.InOutQuad }
                    }
                    Text { anchors.centerIn: parent; text: "⬇"; color: "white"; font.pixelSize: 26 }
                }
                Text {
                    Layout.alignment: Qt.AlignHCenter
                    text: qsTr("松开以安装到 Android")
                    font.pixelSize: 15; font.weight: Font.DemiBold
                    color: Theme.text
                }
                Text {
                    Layout.alignment: Qt.AlignHCenter
                    text: qsTr("支持 .apk / .apks / .xapk")
                    font.pixelSize: 12
                    color: Theme.textDim
                }
            }
        }
    }

    // ===== 危险操作确认框 =====
    Dialog {
        id: confirmDialog
        property string action: ""
        modal: true
        anchors.centerIn: parent
        width: 380
        title: qsTr("确认操作")
        standardButtons: Dialog.Cancel | Dialog.Ok

        Label {
            id: confirmText
            width: parent ? parent.width : 300
            wrapMode: Text.WordWrap
            color: Theme.text
        }
        onAccepted: backend.confirmAction(action)
    }

    // ===== Toast =====
    Rectangle {
        id: toastBar
        visible: false
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottomMargin: 24
        width: Math.min(toastText.implicitWidth + 48, 460)
        height: 40
        radius: 20
        color: Theme.success

        Text {
            id: toastText
            anchors.centerIn: parent
            color: "white"
            font.pixelSize: 13
            font.weight: Font.Medium
        }
    }
}
