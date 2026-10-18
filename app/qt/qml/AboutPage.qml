import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Houttuynia

// 关于 / 许可页（LGPLv3 合规入口）：应用标识 → 许可正文（可滚动卡片）
Item {
    id: page

    // 许可链接胶囊按钮：中性灰底 + 主色文字 + 外链角标，语言与 RButton 一致
    component LinkPill: Rectangle {
        id: pill
        property string label: ""
        signal activated()
        radius: height / 2
        implicitHeight: 28
        implicitWidth: pillText.implicitWidth + 26
        color: pillMa.pressed ? (Theme.dark ? "#3C3C42" : "#DCDCE2")
             : pillMa.containsMouse ? (Theme.dark ? "#38383E" : "#E4E4E9")
             : (Theme.dark ? "#333338" : "#E8E8EC")
        Behavior on color { ColorAnimation { duration: 80 } }
        Text {
            id: pillText
            anchors.centerIn: parent
            text: pill.label + " ↗"
            font.pixelSize: 12; font.weight: Font.Medium
            color: Theme.accent
        }
        MouseArea {
            id: pillMa
            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: pill.activated()
        }
    }

    ScrollView {
        anchors.fill: parent
        contentWidth: availableWidth

        ColumnLayout {
            width: Math.min(parent.width - 48, 560)
            x: (parent.width - width) / 2
            y: 20
            spacing: 14

            Text {
                text: qsTr("关于 / 许可")
                font.pixelSize: 22; font.weight: Font.Bold
                color: Theme.text
            }

            // ===== 应用标识卡（图标/名称/版本胶囊）=====
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: headCol.implicitHeight + 40
                radius: Theme.radius
                color: Theme.card
                border.color: Theme.separator

                ColumnLayout {
                    id: headCol
                    anchors.fill: parent
                    anchors.margins: 20
                    spacing: 4

                    Image {
                        Layout.alignment: Qt.AlignHCenter
                        Layout.preferredWidth: 64
                        Layout.preferredHeight: 64
                        source: "images/logo-appicon.png"
                        fillMode: Image.PreserveAspectFit
                        mipmap: true
                    }
                    Text {
                        Layout.alignment: Qt.AlignHCenter
                        Layout.topMargin: 6
                        text: qsTr("Houttuynia（鱼腥草）")
                        font.pixelSize: 18; font.weight: Font.Bold
                        color: Theme.text
                    }
                    Text {
                        Layout.alignment: Qt.AlignHCenter
                        text: qsTr("在 Mac 上运行 Android 应用")
                        font.pixelSize: 12
                        color: Theme.textDim
                    }
                    Rectangle {
                        Layout.alignment: Qt.AlignHCenter
                        Layout.topMargin: 10
                        width: versionText.implicitWidth + 26
                        height: 24; radius: 12
                        color: Theme.field
                        border.color: Theme.separator; border.width: 1
                        Text {
                            id: versionText
                            anchors.centerIn: parent
                            text: qsTr("版本 %1 · Qt %2").arg(appVersionStr).arg(qtVersionStr)
                            font.pixelSize: 11
                            color: Theme.textDim
                        }
                    }
                }
            }

            Text {
                Layout.topMargin: 6
                text: qsTr("开源许可")
                font.pixelSize: 14; font.weight: Font.Bold
                color: Theme.text
            }

            // Qt LGPL 合规声明
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: qtCardCol.implicitHeight + 28
                radius: Theme.radius
                color: Theme.card
                border.color: Theme.separator

                ColumnLayout {
                    id: qtCardCol
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 8

                    RowLayout {
                        spacing: 8
                        Rectangle {
                            Layout.preferredWidth: 24
                            Layout.preferredHeight: 24
                            radius: 7
                            color: "#41CD52"
                            Text {
                                anchors.centerIn: parent
                                text: "Qt"
                                color: "white"
                                font.pixelSize: 11; font.weight: Font.Bold
                            }
                        }
                        Text {
                            text: qsTr("Qt 6 开源版")
                            font.pixelSize: 13; font.weight: Font.DemiBold
                            color: Theme.text
                        }
                        Item { Layout.fillWidth: true }
                    }
                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: Theme.text
                        font.pixelSize: 12
                        text: qsTr("本程序使用 Qt 开源版（Qt 6，GPL / LGPLv3 双许可）构建，") +
                              qsTr("以动态链接方式使用，未对 Qt 本体做任何修改。")
                    }
                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: Theme.textDim
                        font.pixelSize: 12
                        text: qsTr("Qt 按 LGPLv3（或 GPLv3 / 商业许可，由 Qt 公司提供）授权。") +
                              qsTr("随分发包附带的 Qt 动态库可被替换，对应源码可从 qt.io 获取。")
                    }
                    RowLayout {
                        Layout.topMargin: 4
                        spacing: 8
                        LinkPill {
                            label: qsTr("LGPLv3 完整文本")
                            onActivated: Qt.openUrlExternally(
                                "https://www.gnu.org/licenses/lgpl-3.0.html")
                        }
                        LinkPill {
                            label: qsTr("获取 Qt 源码")
                            onActivated: Qt.openUrlExternally(
                                "https://www.qt.io/download-open-source")
                        }
                        Item { Layout.fillWidth: true }
                    }
                }
            }

            // 本项目许可与商标声明
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: projCardCol.implicitHeight + 28
                radius: Theme.radius
                color: Theme.card
                border.color: Theme.separator

                ColumnLayout {
                    id: projCardCol
                    anchors.fill: parent
                    anchors.margins: 14
                    spacing: 8

                    RowLayout {
                        spacing: 8
                        Image {
                            Layout.preferredWidth: 24
                            Layout.preferredHeight: 24
                            source: "images/logo-appicon.png"
                            fillMode: Image.PreserveAspectFit
                            mipmap: true
                        }
                        Text {
                            text: qsTr("Houttuynia 本体")
                            font.pixelSize: 13; font.weight: Font.DemiBold
                            color: Theme.text
                        }
                        Item { Layout.fillWidth: true }
                    }
                    Text {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: Theme.textDim
                        font.pixelSize: 12
                        text: qsTr("独立开源项目，以 Apache-2.0 许可发布，仅面向合法用途。") +
                              qsTr("Android 是 Google LLC 的商标。")
                    }
                }
            }
        }
    }
}
