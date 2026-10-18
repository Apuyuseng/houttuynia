import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Houttuynia

Item {
    id: page

    function fmtBytes(b) {
        if (!b) return "0 B"
        return b >= 1048576 ? (b / 1048576).toFixed(1) + " MB"
             : b >= 1024 ? (b / 1024).toFixed(1) + " KB" : b + " B"
    }
    function fmtTime(ms) {
        if (!ms) return ""
        return new Date(ms).toLocaleTimeString(Qt.locale(), "HH:mm:ss")
    }
    function statusColor(s) {
        return s >= 500 ? Theme.danger : s >= 400 ? Theme.warning
             : s >= 200 && s < 400 ? Theme.success : Theme.textDim
    }

    // 详情弹窗内容拼装：请求/响应明文
    function buildDetailText() {
        const d = backend.captureDetail
        if (!d || !d.exchanges)
            return qsTr("加载中…")
        const out = []
        const exs = d.exchanges
        for (let i = 0; i < exs.length; i++) {
            const e = exs[i]
            out.push("━━ #" + (i + 1) + " " + (e.method || "") + " " + (e.host || "") + (e.path || "")
                     + " → " + (e.status || "?"))
            if (e.req_head) out.push(qsTr("── 请求头") + "\n" + e.req_head.trim())
            const rb = e.req_body_decoded || e.req_body
            if (rb && rb.text) out.push(qsTr("── 请求体 (") + fmtBytes(rb.size) + ")\n" + rb.text)
            if (e.resp_head) out.push(qsTr("── 响应头") + "\n" + e.resp_head.trim())
            const pb = e.resp_body_decoded || e.resp_body
            if (pb && pb.text) out.push(qsTr("── 响应体 (") + fmtBytes(pb.size) + ")\n" + pb.text)
            out.push("")
        }
        return out.join("\n")
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            Text {
                text: qsTr("流量分析")
                font.pixelSize: 22; font.weight: Font.Bold
                color: Theme.text
            }
            Text {
                text: qsTr("eBPF 明文旁挂（自建 userdebug 镜像）")
                font.pixelSize: 11; color: Theme.textFaint
            }
            Item { Layout.fillWidth: true }
            RButton {
                visible: !backend.captureStatus.running
                text: qsTr("▶ 开始采集")
                highlighted: true
                enabled: backend.vmRunning
                onClicked: backend.captureStart()
            }
            RButton {
                visible: backend.captureStatus.running
                text: qsTr("⏹ 停止采集")
                onClicked: backend.captureStop()
            }
            RButton { text: qsTr("清空数据"); onClicked: backend.captureClear() }
        }

        // 状态卡
        Rectangle {
            Layout.fillWidth: true
            height: 46
            radius: Theme.radius
            color: Theme.card
            border.color: Theme.separator

            RowLayout {
                anchors.fill: parent
                anchors.margins: 14
                spacing: 16

                Rectangle {
                    width: 10; height: 10; radius: 5
                    color: backend.captureStatus.running ? Theme.success : Theme.textFaint
                }
                Text {
                    text: backend.captureStatus.running ? qsTr("采集中") : qsTr("未采集")
                    font.pixelSize: 13; color: Theme.text
                }
                Rectangle { width: 1; height: 18; color: Theme.separator }
                Text {
                    text: qsTr("flow：") + backend.captureTotal
                    font.pixelSize: 12; color: Theme.textDim
                }
                Text {
                    visible: (backend.captureStatus.db_path || "").length > 0
                    text: qsTr("库：") + backend.captureStatus.db_path
                    font.pixelSize: 11; color: Theme.textFaint
                    elide: Text.ElideMiddle
                    Layout.fillWidth: true
                }
            }
        }

        // 过滤行
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            RTextField {
                id: filterHost
                Layout.preferredWidth: 180
                placeholderText: qsTr("host 包含（如 x.com）")
                font.pixelSize: 12
                onAccepted: backend.captureQueryFlows(filterQ.text, text, filterPkg.text)
            }
            RTextField {
                id: filterPkg
                Layout.preferredWidth: 200
                placeholderText: qsTr("应用包名")
                font.family: Theme.mono
                font.pixelSize: 12
                onAccepted: backend.captureQueryFlows(filterQ.text, filterHost.text, text)
            }
            RTextField {
                id: filterQ
                Layout.fillWidth: true
                placeholderText: qsTr("关键词（path / body）")
                font.pixelSize: 12
                onAccepted: backend.captureQueryFlows(text, filterHost.text, filterPkg.text)
            }
            RButton {
                text: qsTr("🔍 查询")
                highlighted: true
                onClicked: backend.captureQueryFlows(filterQ.text, filterHost.text, filterPkg.text)
            }
        }

        // flow 列表
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: Theme.radius
            color: Theme.card
            border.color: Theme.separator

            ListView {
                anchors.fill: parent
                anchors.margins: 6
                clip: true
                spacing: 2
                model: backend.captureFlows

                delegate: Rectangle {
                    width: ListView.view ? ListView.view.width - 8 : 600
                    height: 44
                    radius: 6
                    color: flowMouse.containsMouse ? Theme.cardHover : "transparent"

                    readonly property var flow: modelData

                    RowLayout {
                        anchors.fill: parent
                        anchors.margins: 8
                        spacing: 10

                        Text {
                            text: page.fmtTime(flow.first_ts_ms)
                            font.pixelSize: 11; color: Theme.textFaint
                            font.family: Theme.mono
                        }
                        Rectangle {
                            width: statusLabel.implicitWidth + 12
                            height: 20; radius: 4
                            color: Theme.dark ? "#333338" : "#F0F0F2"
                            Text {
                                id: statusLabel
                                anchors.centerIn: parent
                                text: flow.status || "…"
                                font.pixelSize: 11
                                color: page.statusColor(flow.status)
                                font.weight: Font.DemiBold
                            }
                        }
                        Text {
                            text: flow.method || ""
                            font.pixelSize: 11; color: Theme.text
                            font.weight: Font.Medium
                        }
                        Text {
                            text: (flow.host || "") + (flow.path || "")
                            font.pixelSize: 11; color: Theme.text
                            elide: Text.ElideMiddle
                            Layout.fillWidth: true
                            font.family: Theme.mono
                        }
                        Text {
                            visible: (flow.exchanges || 0) > 1
                            text: "×" + flow.exchanges
                            font.pixelSize: 10; color: Theme.accent
                        }
                        Text {
                            text: "↑" + page.fmtBytes(flow.wbytes) + " ↓" + page.fmtBytes(flow.rbytes)
                            font.pixelSize: 10; color: Theme.textDim
                        }
                        Text {
                            text: flow.comm || flow.package || ""
                            font.pixelSize: 10; color: Theme.textFaint
                            elide: Text.ElideMiddle
                            Layout.maximumWidth: 120
                        }
                    }

                    MouseArea {
                        id: flowMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            backend.captureLoadDetail(flow.id)
                            detailPopup.open()
                        }
                    }
                }
            }

            // 空态
            ColumnLayout {
                anchors.centerIn: parent
                visible: backend.captureFlows.length === 0
                spacing: 8
                Text { text: "🔍"; font.pixelSize: 36; Layout.alignment: Qt.AlignHCenter }
                Text {
                    text: backend.captureStatus.running
                          ? qsTr("暂无数据——在虚拟机里产生流量后自动出现")
                          : qsTr("启动采集后，HTTPS 请求/响应明文将直接落库")
                    font.pixelSize: 13; color: Theme.textDim
                    Layout.alignment: Qt.AlignHCenter
                }
            }
        }
    }

    // 明文详情
    Popup {
        id: detailPopup
        anchors.centerIn: parent
        width: parent ? parent.width * 0.72 : 600
        height: parent ? parent.height * 0.8 : 500
        modal: true
        focus: true
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        background: Rectangle {
            radius: 14; color: Theme.card; border.color: Theme.separator
        }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 16
            spacing: 10

            RowLayout {
                Layout.fillWidth: true
                Text {
                    text: "Flow #" + (backend.captureDetail.id || "")
                    font.pixelSize: 14; font.weight: Font.Bold; color: Theme.text
                }
                Item { Layout.fillWidth: true }
                RButton {
                    text: qsTr("复制")
                    onClicked: {
                        // 剪贴板（Ctrl+C 语义）
                        detailText.selectAll()
                        detailText.copy()
                        detailText.deselect()
                    }
                }
                RButton { text: qsTr("关闭"); onClicked: detailPopup.close() }
            }

            ScrollView {
                Layout.fillWidth: true
                Layout.fillHeight: true

                TextArea {
                    id: detailText
                    readOnly: true
                    wrapMode: TextArea.WrapAnywhere
                    font.family: Theme.mono
                    font.pixelSize: 11
                    color: Theme.text
                    background: Rectangle {
                        radius: 8; color: Theme.dark ? "#1A1A1C" : "#FAFAFB"
                        border.color: Theme.separator
                    }
                    text: page.buildDetailText()
                }
            }
        }
    }
}
