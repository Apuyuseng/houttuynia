import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import QtQuick.Effects
import Houttuynia

Item {
    id: page

    function fmtSizeMB(mb) {
        if (!mb) return ""
        return mb >= 1024 ? (mb / 1024).toFixed(1) + " GB" : Math.round(mb) + " MB"
    }

    // ===== 镜像任务完成态（区分成功/失败展示；失败需用户确认关闭）=====
    // UI 回归钩子：MWINE_DEMO_TASK=1 时用假任务（下载中 40% 态）演示进度卡
    readonly property var demoTask: (typeof demoTaskOn !== "undefined" && demoTaskOn)
        ? ({ running: true, phase: "下载 lineage-gsi-23（1.1 GiB）",
             progress: 451186799, progress_total: 1127967997, done: false, error: "",
             lines: ["↓ lineage-23.2-20260210-UNOFFICIAL-gsi_arm64.img.gz", "断点续传：从 0 B 开始"] })
        : null
    readonly property var imgTask: demoTask || backend.imageTask
    readonly property bool taskRunning: imgTask.running === true
    readonly property string taskError: imgTask.error || ""
    // 刚完成（成功）：短暂展示后自动收起；失败：常驻直到「我知道了」
    property bool taskDoneFlash: false
    Connections {
        target: backend
        function onImageTaskChanged() {
            const t = backend.imageTask
            if (t.running === true) {
                page.taskDoneFlash = false
                flashHide.stop()
            } else if ((t.error || "").length === 0 && t.done === true) {
                page.taskDoneFlash = true
                flashHide.restart()
                // 成功自动收起在线抽屉：让用户直接看到本地列表里的新镜像
                onlineDrawer.close()
            }
        }
    }
    Timer { id: flashHide; interval: 3200; onTriggered: page.taskDoneFlash = false }

    // ===== 现代化任务进度组件 =====
    // 渐变进度条（圆角胶囊 + 渐变填充 + 平滑推进；完成扫光）
    component TaskBar: Rectangle {
        id: bar
        property double frac: 0
        property bool indeterminate: false
        property double shimX: -70
        height: 7
        radius: height / 2
        color: Theme.dark ? Qt.rgba(1, 1, 1, 0.08) : Qt.rgba(0, 0, 0, 0.06)
        clip: true

        Rectangle {
            visible: !bar.indeterminate && bar.frac > 0
            width: Math.max(parent.height, parent.width * Math.min(1, bar.frac))
            height: parent.height
            radius: parent.radius
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0; color: Theme.accent }
                GradientStop { position: 1; color: Qt.lighter(Theme.accent, 1.3) }
            }
            Behavior on width { NumberAnimation { duration: 260; easing.type: Easing.OutCubic } }
            // 完成扫光
            Rectangle {
                visible: bar.frac >= 1
                width: 70; height: parent.height
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0; color: "transparent" }
                    GradientStop { position: 1; color: Qt.rgba(1, 1, 1, 0.55) }
                }
                x: bar.shimX
                NumberAnimation on x {
                    running: visible; loops: Animation.Infinite
                    from: -70; to: bar.width + 70; duration: 1100
                }
            }
        }
        // 不定态：来回滑动的亮胶囊
        Rectangle {
            visible: bar.indeterminate
            width: 64; height: parent.height; radius: parent.radius
            gradient: Gradient {
                orientation: Gradient.Horizontal
                GradientStop { position: 0; color: Qt.darker(Theme.accent, 1.1) }
                GradientStop { position: 1; color: Qt.lighter(Theme.accent, 1.35) }
            }
            SequentialAnimation on x {
                running: visible; loops: Animation.Infinite
                NumberAnimation { to: bar.width - 64; duration: 950; easing.type: Easing.InOutQuad }
                NumberAnimation { to: 0; duration: 950; easing.type: Easing.InOutQuad }
            }
        }
    }

    // 阶段徽标（phase 关键词 → 小胶囊：下载/解压/校验/组装/安装…）
    component PhaseChip: Rectangle {
        // 纯图标小圆片：phase 标题文本已含阶段字样（如"下载 xxx"），
        // 徽标再带字就是重复——只留图标做色彩点缀
        readonly property var kinds: [
            { match: "下载", label: "⬇", color: "#3B82F6" },
            { match: "解压", label: "📦", color: "#8B5CF6" },
            { match: "校验", label: "🔐", color: "#F59E0B" },
            { match: "组装", label: "🧩", color: "#EC4899" },
            { match: "稀疏", label: "🧱", color: "#14B8A6" },
            { match: "安装", label: "💾", color: "#10B981" },
            { match: "导入", label: "📥", color: "#3B82F6" },
        ]
        property string phase: ""
        readonly property var kind: {
            for (let i = 0; i < kinds.length; ++i)
                if (phase.indexOf(kinds[i].match) >= 0) return kinds[i]
            return null
        }
        visible: kind !== null && phase.length > 0
        radius: height / 2
        height: 20
        width: 20
        color: Qt.rgba(kind ? kind.color : "#888", Theme.dark ? 0.28 : 0.12)
        border.width: 1
        border.color: kind ? Qt.rgba(kind.color, 0.45) : "transparent"
        Text {
            id: chipText
            anchors.centerIn: parent
            text: parent.visible && kind ? kind.label : ""
            font.pixelSize: 11; font.weight: Font.DemiBold
            color: kind ? kind.color : Theme.text
        }
    }
    // UI 回归钩子：MWINE_OPEN_DRAWER=1 启动即打开在线镜像抽屉
    Component.onCompleted: {
        if (typeof openDrawerOnStart !== "undefined" && openDrawerOnStart)
            onlineDrawer.open()
    }

    // ===== 切换浮层状态（现代化提示：开始/成功/失败）=====
    property string switchState: "idle"   // idle | running | success | error
    property string switchErrorText: ""
    Connections {
        target: backend
        function onImageSwitchStarted() { page.switchState = "running" }
        function onImageSwitchCancelled() { page.switchState = "idle" }
        function onImageSwitchFinished(ok, error) {
            page.switchErrorText = error || ""
            page.switchState = ok ? "success" : "error"
            if (ok) switchAutoHide.restart()
        }
    }
    Timer { id: switchAutoHide; interval: 2600; onTriggered: page.switchState = "idle" }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            Text {
                text: qsTr("镜像")
                font.pixelSize: 22; font.weight: Font.Bold
                color: Theme.text
            }
            Item { Layout.fillWidth: true }
            RButton {
                text: qsTr("☁️ 在线镜像")
                onClicked: onlineDrawer.open()
            }
            RButton {
                text: qsTr("📦 导入分发包…")
                onClicked: imagePicker.open()
            }
        }

        // ===== 镜像任务卡（进度环 + 渐变条 + 阶段徽标；成功闪现 / 失败须确认）=====
        Rectangle {
            visible: page.taskRunning || page.taskDoneFlash || page.taskError.length > 0
            Layout.fillWidth: true
            height: page.taskError.length > 0 ? 132 : 100
            radius: Theme.radius
            color: Theme.card
            border.color: page.taskError.length > 0 ? Theme.danger : Theme.separator

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 14
                spacing: 8

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    // 终态状态符（成功✓ / 失败✕）；进行中的活跃感由
                    // 进度条（定量填充 / 不定态滑条）与阶段徽标表达，不再放旋转点
                    Text {
                        visible: !page.taskRunning
                        text: page.taskError.length > 0 ? "✕" : "✓"
                        color: page.taskError.length > 0 ? Theme.danger : Theme.success
                        font.pixelSize: 16; font.weight: Font.Bold
                    }

                    PhaseChip { phase: page.imgTask.phase || "" }

                    Text {
                        visible: page.taskError.length === 0
                        Layout.fillWidth: true
                        text: {
                            const t = page.imgTask
                            if (page.taskRunning)
                                return (t.phase || qsTr("处理中…"))
                            return qsTr("已安装到本地镜像库 · 列表已刷新")
                        }
                        font.pixelSize: 13; font.weight: Font.DemiBold
                        color: Theme.text
                        elide: Text.ElideMiddle
                    }
                    Text {
                        visible: page.taskError.length > 0
                        Layout.fillWidth: true
                        text: qsTr("镜像任务失败")
                        font.pixelSize: 13; font.weight: Font.DemiBold
                        color: Theme.danger
                        elide: Text.ElideMiddle
                    }

                    // 数值（右对齐：x / y MB + 百分比）
                    Text {
                        visible: page.taskRunning
                                 && (page.imgTask.progress_total || 0) > 0
                        text: ((page.imgTask.progress || 0) / page.imgTask.progress_total * 100).toFixed(0)
                              + "% · "
                              + ((page.imgTask.progress || 0) / 1048576).toFixed(0)
                              + " / "
                              + (page.imgTask.progress_total / 1048576).toFixed(0)
                              + " MB"
                        font.pixelSize: 12; color: Theme.textDim
                        font.weight: Font.DemiBold
                    }

                    RButton {
                        visible: page.taskRunning
                        text: qsTr("取消")
                        onClicked: backend.imageCancelTask()
                    }
                    // 失败：用户确认后才关闭（轮询回放已由 daemon ack 阻断）
                    RButton {
                        visible: page.taskError.length > 0
                        text: qsTr("我知道了")
                        highlighted: true
                        onClicked: backend.ackImageTask()
                    }
                }
                // 渐变进度条 / 不定态滑条（唯一进度指示）
                TaskBar {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 7
                    visible: page.taskRunning || page.taskDoneFlash
                    frac: (page.imgTask.progress_total || 0) > 0
                          ? Math.min(1, (page.imgTask.progress || 0)
                                       / page.imgTask.progress_total)
                          : (page.taskDoneFlash ? 1 : 0)
                    indeterminate: page.taskRunning
                                   && (page.imgTask.progress_total || 0) <= 0
                }

                // 错误详情（失败常驻）/ 日志尾（进行中）
                ScrollView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    visible: page.taskError.length > 0
                    TextArea {
                        readOnly: true
                        wrapMode: TextArea.WrapAnywhere
                        font.family: Theme.mono
                        font.pixelSize: 11
                        color: Theme.danger
                        background: null
                        text: page.taskError + "\n"
                              + (page.imgTask.lines || []).slice(-4).join("\n")
                    }
                }
                TextArea {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    visible: page.taskRunning
                    readOnly: true
                    wrapMode: TextArea.WrapAnywhere
                    font.family: Theme.mono
                    font.pixelSize: 11
                    color: Theme.textDim
                    background: null
                    text: (page.imgTask.lines || []).slice(-3).join("\n")
                }
            }
        }

        // ===== 本地镜像列表（常驻；在线镜像在右侧抽屉，不再互相顶替）=====
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: Theme.radius
            color: Theme.card
            border.color: Theme.separator

            ListView {
                anchors.fill: parent
                anchors.margins: 6
                model: backend.images
                spacing: 4
                clip: true

                delegate: Rectangle {
                    width: ListView.view ? ListView.view.width - 8 : 400
                    height: 62
                    radius: 8
                    color: useMouse.containsMouse ? Theme.cardHover : "transparent"

                    readonly property var img: modelData

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        spacing: 12

                        Text {
                            text: img.in_use ? "✅" : "💿"
                            font.pixelSize: 20
                        }
                        ColumnLayout {
                            spacing: 2
                            Text {
                                text: (img.display || img.api) + " · " + img.name
                                font.pixelSize: 13; font.weight: Font.DemiBold
                                color: Theme.text
                                elide: Text.ElideMiddle
                                Layout.fillWidth: true
                            }
                            Text {
                                text: img.rel + (img.size_mb ? " · " + page.fmtSizeMB(img.size_mb) : "")
                                font.pixelSize: 11; color: Theme.textDim
                                elide: Text.ElideMiddle
                                Layout.fillWidth: true
                            }
                        }
                        Item { Layout.fillWidth: true }
                        Text {
                            visible: img.in_use
                            text: qsTr("使用中"); font.pixelSize: 12; color: Theme.success
                        }
                        RButton {
                            visible: !img.in_use
                            text: qsTr("切换")
                            highlighted: true
                            onClicked: backend.useImage(img.path)
                        }
                        RButton {
                            visible: !img.in_use
                            text: qsTr("删除")
                            onClicked: {
                                delImageDialog.path = img.path
                                delImageDialog.open()
                            }
                        }
                    }

                    MouseArea {
                        id: useMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.NoButton
                    }
                }
            }
        }

        Text {
            visible: backend.images.length === 0
            text: qsTr("暂无本地镜像：点「在线镜像」拉取官方/社区镜像，或导入 houttuynia-image-*.tar.zst")
            font.pixelSize: 12; color: Theme.textDim
        }
    }

    // ===== 在线镜像抽屉（右侧滑出；本地列表常驻底层可见）=====
    Drawer {
        id: onlineDrawer
        edge: Qt.RightEdge
        width: Math.min(480, parent.width - 80)
        height: parent.height
        padding: 0
        modal: true
        dim: true
        opacity: 0.98

        background: Rectangle {
            radius: 16
            color: Theme.dark ? "#222226" : "#F6F6F8"
            border.color: Theme.separator
            Rectangle {
                anchors.left: parent.left; anchors.right: parent.right
                anchors.top: parent.top; anchors.topMargin: 24; height: 1
                color: Theme.separator; visible: false
            }
        }

        onOpened: {
            // 每次打开都重新拉取（后端/目录有新增时用户能看到最新目录）
            if (!backend.officialLoading)
                backend.officialImagesLoad(officialFilter.text)
        }

        contentItem: ColumnLayout {
            spacing: 10

            // 抽屉头：标题 + 过滤 + 刷新 + 关闭
            RowLayout {
                Layout.leftMargin: 18
                Layout.rightMargin: 14
                Layout.topMargin: 16
                Layout.fillWidth: true
                spacing: 8
                Text {
                    text: qsTr("在线镜像")
                    font.pixelSize: 17; font.weight: Font.DemiBold
                    color: Theme.text
                }
                Text {
                    text: qsTr("官方 + 社区 · arm64")
                    font.pixelSize: 11; color: Theme.textDim
                }
                Item { Layout.fillWidth: true }
                BusyIndicator {
                    visible: backend.officialLoading
                    running: visible
                    implicitWidth: 16; implicitHeight: 16
                }
                RButton {
                    text: qsTr("刷新")
                    onClicked: backend.officialImagesLoad(officialFilter.text)
                }
                Text {
                    text: "✕"
                    font.pixelSize: 13; color: Theme.textDim
                    MouseArea {
                        anchors.fill: parent
                        anchors.margins: -8
                        cursorShape: Qt.PointingHandCursor
                        onClicked: onlineDrawer.close()
                    }
                }
            }
            RTextField {
                id: officialFilter
                Layout.leftMargin: 18
                Layout.rightMargin: 18
                Layout.fillWidth: true
                placeholderText: qsTr("搜索 / API 过滤（如 34、lineage）")
                onAccepted: backend.officialImagesLoad(text)
            }

            ListView {
                Layout.leftMargin: 10
                Layout.rightMargin: 10
                Layout.topMargin: 2
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 6
                model: backend.officialImages

                ScrollBar.vertical: ScrollBar { }

                delegate: Rectangle {
                    width: ListView.view ? ListView.view.width - 4 : 420
                    height: 64
                    radius: 10
                    color: pullMouse.containsMouse ? Theme.cardHover
                         : (Theme.dark ? Qt.rgba(1,1,1,0.04) : Qt.rgba(0,0,0,0.03))
                    border.color: community ? "#5BA86B40" : Theme.separator

                    readonly property var img: modelData
                    readonly property bool community: img.source === "community"

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        spacing: 10

                        Text { text: community ? "🌱" : "🤖"; font.pixelSize: 20 }

                        ColumnLayout {
                            spacing: 2
                            Layout.fillWidth: true
                            RowLayout {
                                spacing: 6
                                Text {
                                    text: community ? qsTr("社区") : qsTr("官方")
                                    font.pixelSize: 10
                                    color: community ? "#5BA86B" : Theme.textDim
                                }
                                Text {
                                    visible: community
                                    text: qsTr("· 需本地同版本基底")
                                    font.pixelSize: 10; color: Theme.textFaint
                                }
                            }
                            Text {
                                Layout.fillWidth: true
                                text: img.display || ""
                                font.pixelSize: 13; font.weight: Font.DemiBold
                                color: Theme.text
                                elide: Text.ElideMiddle
                                wrapMode: Text.WordWrap
                                maximumLineCount: 2
                            }
                            Text {
                                text: (img.size / 1048576 >= 1024
                                       ? (img.size / 1073741824).toFixed(2) + " GB"
                                       : Math.round(img.size / 1048576) + " MB")
                                      + (community ? qsTr(" · 拉取时以本地基底组装") : "")
                                font.pixelSize: 11; color: Theme.textDim
                            }
                        }
                        RButton {
                            text: qsTr("拉取")
                            highlighted: true
                            onClicked: backend.officialPull(img.path)
                        }
                    }

                    MouseArea {
                        id: pullMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.NoButton
                    }
                }
            }
        }
    }

    // ===== 切换镜像浮层（现代化居中卡：进行中/成功/失败）=====
    Rectangle {
        visible: page.switchState !== "idle"
        anchors.fill: parent
        color: Qt.rgba(0, 0, 0, 0.32)
        z: 60

        Rectangle {
            anchors.centerIn: parent
            width: 340
            height: page.switchState === "error" ? 210 : 180
            radius: 18
            color: Theme.dark ? "#2A2A30" : "#FFFFFF"
            border.color: page.switchState === "error" ? Theme.danger : Theme.separator

            ColumnLayout {
                anchors.centerIn: parent
                spacing: 10

                BusyIndicator {
                    visible: page.switchState === "running"
                    running: visible
                    implicitWidth: 42; implicitHeight: 42
                    Layout.alignment: Qt.AlignHCenter
                }
                Rectangle {
                    visible: page.switchState !== "running"
                    width: 46; height: 46; radius: 23
                    color: page.switchState === "success" ? Theme.success : Theme.danger
                    Layout.alignment: Qt.AlignHCenter
                    scale: 1
                    Text {
                        anchors.centerIn: parent
                        text: page.switchState === "success" ? "✓" : "✕"
                        color: "white"; font.pixelSize: 24; font.weight: Font.Bold
                    }
                    SequentialAnimation on scale {
                        running: visible
                        NumberAnimation { to: 1.12; duration: 160; easing.type: Easing.OutQuad }
                        NumberAnimation { to: 1; duration: 220; easing.type: Easing.OutBack }
                    }
                }
                Text {
                    Layout.alignment: Qt.AlignHCenter
                    text: page.switchState === "running" ? qsTr("正在切换镜像")
                         : page.switchState === "success" ? qsTr("已切换镜像")
                         : qsTr("切换失败")
                    font.pixelSize: 16; font.weight: Font.DemiBold
                    color: page.switchState === "error" ? Theme.danger : Theme.text
                }
                Text {
                    Layout.alignment: Qt.AlignHCenter
                    visible: page.switchState === "running"
                    text: qsTr("正在存档并重启虚拟机，全程约 10–60 秒")
                    font.pixelSize: 12; color: Theme.textDim
                }
                Text {
                    visible: page.switchState === "success"
                    Layout.alignment: Qt.AlignHCenter
                    text: qsTr("新镜像已生效")
                    font.pixelSize: 12; color: Theme.textDim
                }
                Text {
                    visible: page.switchState === "error"
                    Layout.alignment: Qt.AlignHCenter
                    Layout.maximumWidth: 290
                    wrapMode: Text.WordWrap
                    text: page.switchErrorText
                    font.pixelSize: 12; color: Theme.danger
                }
                RButton {
                    visible: page.switchState === "error"
                    Layout.alignment: Qt.AlignHCenter
                    text: qsTr("知道了")
                    highlighted: true
                    onClicked: page.switchState = "idle"
                }
            }
        }
    }

    Dialog {
        id: delImageDialog
        property string path: ""
        modal: true
        anchors.centerIn: parent
        title: qsTr("删除镜像")
        standardButtons: Dialog.Cancel | Dialog.Yes
        width: 380
        Label {
            width: parent ? parent.width : 300
            wrapMode: Text.WordWrap
            color: Theme.text
            text: qsTr("确定删除该镜像？切换中的镜像不可删除。")
        }
        onAccepted: backend.deleteImage(path)
    }

    FileDialog {
        id: imagePicker
        title: qsTr("选择镜像分发包")
        nameFilters: [qsTr("镜像包") + " (*.tar.zst *.zip)"]
        onAccepted: backend.importImage(selectedFile.toString())
    }
}
