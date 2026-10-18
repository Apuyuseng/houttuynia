import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Houttuynia

Item {
    id: page

    property string pkg: ""

    // 应用库右键「配置按键映射」跳转过来的预选包名
    onVisibleChanged: {
        if (!visible) return
        let n = page
        while (n && !(n instanceof ApplicationWindow)) n = n.parent
        if (n && n.selectedKeysPkg && n.selectedKeysPkg.length > 0) {
            pkg = n.selectedKeysPkg
            n.selectedKeysPkg = ""
        }
        backend.refreshForeground()
    }

    // 候选包列表：已装应用 ∪ 已有档案（去重）
    property var pkgCandidates: {
        const seen = {}
        const list = []
        for (let i = 0; i < backend.apps.length; i++) {
            const p = backend.apps[i].package
            if (p && !seen[p]) { seen[p] = true; list.push(p) }
        }
        for (let i = 0; i < backend.keyProfiles.length; i++) {
            const p = backend.keyProfiles[i].package
            if (p && !seen[p]) { seen[p] = true; list.push(p) }
        }
        return list
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 14

        RowLayout {
            Layout.fillWidth: true
            Text {
                text: qsTr("按键映射")
                font.pixelSize: 22; font.weight: Font.Bold
                color: Theme.text
            }
            Item { Layout.fillWidth: true }
            RButton {
                text: qsTr("📁 方案管理（") + backend.keyProfiles.length + qsTr("）")
                onClicked: profilesDialog.open()
            }
            RButton {
                // 包名截断显示，避免按钮把标题行撑爆
                text: backend.keywatcherRunning
                      ? qsTr("⏹ 停用映射（") + (backend.keywatcherPkg.length > 22
                            ? backend.keywatcherPkg.substring(0, 22) + "…"
                            : backend.keywatcherPkg) + qsTr("）")
                      : qsTr("▶ 启用映射")
                highlighted: backend.keywatcherRunning
                enabled: pkg.length > 0
                onClicked: backend.keywatcherRunning ? backend.keywatchStop()
                                                     : backend.keywatchStart(pkg)
            }
        }

        // 目标应用选择
        Rectangle {
            Layout.fillWidth: true
            height: 64
            radius: Theme.radius
            color: Theme.card
            border.color: Theme.separator

            RowLayout {
                anchors.fill: parent
                anchors.margins: 14
                spacing: 12

                Text { text: qsTr("🎯 目标应用"); font.pixelSize: 13; color: Theme.text }

                RTextField {
                    id: pkgField
                    // 弹性宽度（140–300）：窄窗口下收缩，避免整行溢出
                    Layout.fillWidth: true
                    Layout.minimumWidth: 140
                    Layout.maximumWidth: 300
                    placeholderText: qsTr("包名（com.example.app）")
                    text: page.pkg
                    font.family: Theme.mono
                    font.pixelSize: 12
                    onTextChanged: page.pkg = text.trim()
                    onAccepted: page.pkg = text.trim()
                }

                RComboBox {
                    id: pkgPick
                    Layout.fillWidth: true
                    Layout.minimumWidth: 120
                    Layout.maximumWidth: 240
                    visible: page.pkgCandidates.length > 0
                    model: page.pkgCandidates
                    displayText: qsTr("选择已装应用…")
                    onActivated: function(index) {
                        pkgField.text = model[index]
                        page.pkg = model[index]
                    }
                }

                Text {
                    visible: backend.foregroundPkg && backend.foregroundPkg.length > 0
                    text: qsTr("前台：") + backend.foregroundPkg
                    font.pixelSize: 11; color: Theme.textDim
                    elide: Text.ElideMiddle
                    // 限宽后 elide 才生效（无约束时按内容宽撑开整行）
                    Layout.maximumWidth: 170
                    Layout.minimumWidth: 0
                }
                RButton {
                    text: qsTr("⤵ 用前台应用")
                    visible: backend.foregroundPkg && backend.foregroundPkg.length > 0
                    onClicked: { pkgField.text = backend.foregroundPkg; page.pkg = backend.foregroundPkg }
                }
            }
        }

        // 内置模板：下拉选择 + 套用
        Rectangle {
            Layout.fillWidth: true
            height: 56
            radius: Theme.radius
            color: Theme.card
            border.color: Theme.separator

            RowLayout {
                anchors.fill: parent
                anchors.margins: 14
                spacing: 12

                Text { text: qsTr("📋 内置模板"); font.pixelSize: 13; color: Theme.text }

                RComboBox {
                    id: tplPick
                    Layout.fillWidth: true
                    Layout.minimumWidth: 160
                    Layout.maximumWidth: 380
                    model: backend.keyTemplates.map(t => t.name || t.id)
                    displayText: currentIndex >= 0 && currentIndex < backend.keyTemplates.length
                                 ? backend.keyTemplates[currentIndex].name || backend.keyTemplates[currentIndex].id
                                 : qsTr("选择模板…")
                    onActivated: function(index) { /* 仅记录选择，点「套用」生效 */ }
                }

                RButton {
                    text: qsTr("套用")
                    highlighted: true
                    enabled: page.pkg.length > 0 && tplPick.currentIndex >= 0
                    onClicked: backend.keysApplyTemplate(
                        page.pkg, backend.keyTemplates[tplPick.currentIndex].id)
                }

                Text {
                    text: qsTr("套用后可用命令行可视化微调；` 键随时总开关；首次启用需授权「辅助功能」")
                    font.pixelSize: 11; color: Theme.textDim
                    elide: Text.ElideMiddle
                    Layout.fillWidth: true
                    Layout.minimumWidth: 120
                }
            }
        }

        // 画面校准截图（全宽）
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: Theme.radius
            color: Theme.card
            border.color: Theme.separator

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 8

                RowLayout {
                    Layout.fillWidth: true
                    Text {
                        text: qsTr("画面校准截图")
                        font.pixelSize: 13; font.weight: Font.DemiBold; color: Theme.text
                    }
                    Item { Layout.fillWidth: true }
                    RButton {
                        text: qsTr("📷 截取当前画面")
                        enabled: page.pkg.length > 0 && backend.vmRunning
                        onClicked: backend.keysShot(page.pkg)
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    radius: 8
                    color: Theme.dark ? "#1A1A1C" : "#F4F4F6"
                    border.color: Theme.separator

                    Image {
                        id: shotImage
                        anchors.fill: parent
                        anchors.margins: 8
                        fillMode: Image.PreserveAspectFit
                        asynchronous: true
                        source: backend.keyShotPath && backend.keyShotPath.length > 0
                                ? "file://" + backend.keyShotPath : ""
                    }

                    Text {
                        anchors.centerIn: parent
                        visible: shotImage.status !== Image.Ready
                        text: backend.keyShotPath && backend.keyShotPath.length > 0
                              ? qsTr("加载截图…") : qsTr("截取游戏画面后在此显示，用于对照布置按键位置")
                        font.pixelSize: 12; color: Theme.textFaint
                    }
                }

                Text {
                    text: qsTr("完整可视化节点编辑器（摇杆 / 技能 / 连点 / 宏）通过命令行使用：") + "\n" +
                          "mwine keys template  # " + qsTr("查看内置模板") + "\n" +
                          "mwine keys get/set <" + qsTr("包名") + ">  # " + qsTr("导出 / 导入 JSON 档案")
                    font.pixelSize: 11; color: Theme.textDim
                    font.family: Theme.mono
                    wrapMode: Text.WordWrap
                    Layout.fillWidth: true
                }
            }
        }
    }

    // 方案管理弹框（已保存档案：载入 / 删除）
    Dialog {
        id: profilesDialog
        modal: true
        parent: Overlay.overlay
        anchors.centerIn: parent
        width: Math.min(560, page.width - 80)
        height: 400
        padding: 0

        background: Rectangle {
            radius: Theme.radius
            color: Theme.card
            border.color: Theme.separator
        }

        header: Rectangle {
            color: "transparent"
            implicitHeight: 54
            radius: Theme.radius
            Text {
                anchors.left: parent.left; anchors.leftMargin: 18
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("已保存方案（") + backend.keyProfiles.length + qsTr("）")
                font.pixelSize: 15; font.weight: Font.DemiBold; color: Theme.text
            }
            ToolButton {
                anchors.right: parent.right; anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                text: "✕"
                font.pixelSize: 13
                onClicked: profilesDialog.close()
            }
        }

        contentItem: Rectangle {
            color: "transparent"
            ListView {
                anchors.fill: parent
                anchors.margins: 12
                clip: true
                spacing: 4
                model: backend.keyProfiles

                delegate: Rectangle {
                    width: ListView.view ? ListView.view.width : 400
                    height: 44
                    radius: 8
                    color: rowMouse.containsMouse ? Theme.cardHover : "transparent"

                    readonly property var prof: modelData

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 10; anchors.rightMargin: 8
                        spacing: 10

                        ColumnLayout {
                            spacing: 1
                            Layout.fillWidth: true
                            Text {
                                text: prof.package
                                font.pixelSize: 12; font.family: Theme.mono
                                color: Theme.text
                                elide: Text.ElideMiddle
                                Layout.fillWidth: true
                            }
                            Text {
                                text: (prof.name || "") + " · " + (prof.nodes || 0) + qsTr(" 节点")
                                font.pixelSize: 10; color: Theme.textDim
                                elide: Text.ElideMiddle
                                Layout.fillWidth: true
                            }
                        }
                        RButton {
                            text: qsTr("载入")
                            onClicked: {
                                pkgField.text = prof.package
                                page.pkg = prof.package
                                profilesDialog.close()
                            }
                        }
                        RButton {
                            text: qsTr("删除")
                            onClicked: backend.keysDelete(prof.package)
                        }
                    }

                    MouseArea {
                        id: rowMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.NoButton
                    }
                }

                Text {
                    anchors.centerIn: parent
                    visible: backend.keyProfiles.length === 0
                    text: qsTr("暂无保存方案\n套用模板并启用映射后自动生成")
                    horizontalAlignment: Text.AlignHCenter
                    font.pixelSize: 12; color: Theme.textDim
                }
            }
        }
    }
}
