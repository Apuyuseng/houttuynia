import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import Houttuynia

// 真机连接：四种方式并存——出码配对（Android Studio 同款，最现代）/
// 配对码 / USB 即插即用 / 手动 IP:端口（含 USB 一键转无线）。配对一次后
// 同 Wi-Fi 自动回连（ADB_MDNS_AUTO_CONNECT，由守护拉起的 adb server 继承）。
Item {
    id: page

    property var pairingSvcs: []   // mDNS 发现的 _adb-tls-pairing 服务（对象数组）
    property var pairingLabels: [] // 对应下拉文案
    property var hostPairRows: []  // 出码配对二维码矩阵（逐行 '0'/'1'）
    readonly property string hostPairState: backend.deviceHostPair.state || ""

    onVisibleChanged: {
        backend.setDevicesActive(visible)
        if (visible)
            backend.deviceRefresh()
    }

    // APK 选择器：安装到指定真机（property 传目标 serial）
    FileDialog {
        id: apkDialog
        property string targetSerial: ""
        nameFilters: [qsTr("Android 安装包 (*.apk)"), qsTr("所有文件 (*)")]
        onAccepted: {
            if (targetSerial.length > 0)
                backend.installApkTo(selectedFile.toString(), targetSerial)
        }
    }

    // 出码配对弹窗：Mac 显示二维码，手机扫一下（与 Android Studio 无线配对同款）。
    // 守护后台等 mDNS 出现指定配对服务实例 → adb pair → 自动连接；状态轮询驱动 UI。
    Dialog {
        id: hostPairDialog
        modal: true
        title: qsTr("扫码配对真机")
        width: 380
        standardButtons: Dialog.Close

        ColumnLayout {
            id: dialogCol
            width: parent ? parent.width : 340
            spacing: 12

            // 二维码（守护给的矩阵逐行 '0'/'1'；'1' = 黑模块，四周补 4 模块静区）
            Canvas {
                id: qrCanvas
                Layout.alignment: Qt.AlignHCenter
                width: 248
                height: 248
                onPaint: {
                    const ctx = getContext("2d")
                    ctx.fillStyle = "#FFFFFF"
                    ctx.fillRect(0, 0, width, height)
                    const rows = page.hostPairRows
                    if (!rows || rows.length === 0)
                        return
                    const n = rows.length, quiet = 4
                    const cell = width / (n + quiet * 2)
                    ctx.fillStyle = "#000000"
                    for (let r = 0; r < n; ++r) {
                        const row = rows[r]
                        for (let c = 0; c < n; ++c)
                            if (row[c] === "1")
                                ctx.fillRect((c + quiet) * cell, (r + quiet) * cell,
                                             cell + 0.5, cell + 0.5)
                    }
                }
            }

            // 无矩阵平台兜底：展示载荷文本（可手动照抄配对码路径）
            TextEdit {
                Layout.fillWidth: true
                visible: page.hostPairRows.length === 0 && page.hostPairState !== ""
                text: backend.deviceHostPair.qr || ""
                wrapMode: TextEdit.WrapAnywhere
                font.pixelSize: 11
                font.family: "Menlo"
                readOnly: true
                selectByMouse: true
                color: Theme.text
            }

            Text {
                Layout.fillWidth: true
                text: qsTr("手机与 Mac 连同一 Wi-Fi，且已开启「无线调试」（开发者选项，一次性）："
                           + "设置 → 开发者选项 → 无线调试 → 「使用二维码配对设备」，"
                           + "扫描上方二维码即可。")
                wrapMode: Text.WordWrap
                font.pixelSize: 12
                color: Theme.textFaint
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 10
                BusyIndicator {
                    visible: page.hostPairState === "" || page.hostPairState === "waiting"
                             || page.hostPairState === "pairing"
                             || page.hostPairState === "connecting"
                    running: visible
                    Layout.preferredWidth: 22
                    Layout.preferredHeight: 22
                }
                Text {
                    Layout.fillWidth: true
                    text: page.hostPairStatusText()
                    wrapMode: Text.WordWrap
                    font.pixelSize: 13
                    font.weight: Font.Medium
                    color: page.hostPairState === "done" ? Theme.success
                         : page.hostPairState === "failed" ? Theme.warning
                         : Theme.text
                }
            }

            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                spacing: 10
                RButton {
                    visible: page.hostPairState === "waiting"
                    text: qsTr("取消等待")
                    onClicked: backend.deviceHostPairCancel()
                }
                RButton {
                    visible: page.hostPairState === "failed" || page.hostPairState === "cancelled"
                    highlighted: true
                    text: qsTr("重新生成二维码")
                    onClicked: backend.deviceHostPairBegin()
                }
            }
        }
    }

    Connections {
        target: backend
        function onDevicesChanged() { page.refreshPairingSvcs() }
        function onDeviceHostPairChanged() {
            const hp = backend.deviceHostPair
            page.hostPairRows = hp.rows || []
            qrCanvas.requestPaint()
            if (page.hostPairState === "done" && hostPairDialog.visible)
                hostPairDialog.close()
        }
    }
    // UI 回归钩子：MWINE_QR_DIALOG=1 启动即 begin + 打开出码配对弹窗（截图回归）
    Component.onCompleted: {
        refreshPairingSvcs()
        if (typeof openQrDialogOnStart !== "undefined" && openQrDialogOnStart) {
            backend.deviceHostPairBegin()
            hostPairDialog.open()
        }
    }

    function refreshPairingSvcs() {
        const svcs = [], labels = [qsTr("自动选择唯一配对服务")]
        for (let i = 0; i < backend.deviceDiscovered.length; ++i) {
            const s = backend.deviceDiscovered[i]
            if (String(s.type).indexOf("_adb-tls-pairing") === 0) {
                svcs.push(s)
                labels.push(s.instance + "（" + s.addr + "）")
            }
        }
        pairingSvcs = svcs
        pairingLabels = labels
        if (svcCombo.currentIndex > labels.length - 1)
            svcCombo.currentIndex = 0
    }

    // 出码配对会话状态 → 文案
    function hostPairStatusText() {
        const msg = backend.deviceHostPair.message || ""
        switch (hostPairState) {
        case "": return qsTr("正在生成二维码…")
        case "waiting": return qsTr("等待手机扫码…（二维码 3 分钟内有效）")
        case "pairing": return qsTr("已发现设备，配对中…")
        case "connecting": return qsTr("配对成功，连接中…")
        case "done": return qsTr("✓ ") + msg
        case "failed": return qsTr("✗ ") + (msg || qsTr("配对失败"))
        case "cancelled": return qsTr("已取消")
        }
        return ""
    }

    ScrollView {
        anchors.fill: parent
        contentWidth: availableWidth

        ColumnLayout {
            width: Math.min(parent.width, 860)
            spacing: 14

            RowLayout {
                Layout.topMargin: 24
                Layout.leftMargin: 24
                Layout.rightMargin: 24
                spacing: 10
                Text {
                    text: qsTr("真机连接")
                    font.pixelSize: 22; font.weight: Font.Bold
                    color: Theme.text
                }
                Item { Layout.fillWidth: true }
                Text {
                    text: qsTr("虚拟机之外，直接连接你的 Android 手机 / 平板")
                    font.pixelSize: 12
                    color: Theme.textFaint
                }
            }

            // ===== 四种连接方式 =====
            RowLayout {
                Layout.leftMargin: 24
                Layout.rightMargin: 24
                Layout.fillWidth: true
                spacing: 14

                // 方式一：出码配对（Android Studio 同款，主推）
                RGroupBox {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    title: qsTr("① 扫码配对（推荐，Android 11+）")
                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 10
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("与 Android Studio「使用 Wi-Fi 配对设备」同款机制（ADB 无线调试）："
                                       + "Mac 显示二维码、手机扫码。首次需在手机开启一次「无线调试」"
                                       + "开关（开发者选项内），之后扫一下即自动配对并连接。")
                            wrapMode: Text.WordWrap
                            font.pixelSize: 12
                            color: Theme.textFaint
                        }
                        RButton {
                            Layout.alignment: Qt.AlignHCenter
                            text: qsTr("📱 显示配对二维码")
                            highlighted: true
                            onClicked: {
                                backend.deviceHostPairBegin()
                                hostPairDialog.open()
                            }
                        }
                        Text {
                            Layout.alignment: Qt.AlignHCenter
                            visible: page.hostPairState === "waiting"
                                     || page.hostPairState === "pairing"
                                     || page.hostPairState === "connecting"
                            text: qsTr("配对进行中…（关闭窗口也不影响，完成会提示）")
                            font.pixelSize: 11
                            color: Theme.accent
                        }
                    }
                }

                // 方式二：配对码（手机弹窗里的 6 位数字）
                RGroupBox {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    title: qsTr("② 输入配对码")
                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 10

                        Text {
                            Layout.fillWidth: true
                            text: qsTr("手机「无线调试 → 使用配对码配对设备」弹窗里的 6 位数字。"
                                       + "手机与 Mac 需在同一 Wi-Fi。")
                            wrapMode: Text.WordWrap
                            font.pixelSize: 12
                            color: Theme.textFaint
                        }
                        RComboBox {
                            id: svcCombo
                            Layout.fillWidth: true
                            visible: page.pairingSvcs.length > 0
                            model: page.pairingLabels
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            RTextField {
                                id: codeField
                                Layout.preferredWidth: 110
                                placeholderText: qsTr("6 位配对码")
                                font.pixelSize: 16
                                horizontalAlignment: TextInput.AlignHCenter
                                validator: RegularExpressionValidator { regularExpression: /[0-9A-Za-z]{0,10}/ }
                                onAccepted: pairBtn.clicked()
                            }
                            RTextField {
                                id: manualAddrField
                                Layout.fillWidth: true
                                placeholderText: qsTr("可选：手输 ip:配对端口（mDNS 未发现时）")
                                font.pixelSize: 13
                            }
                        }
                        RButton {
                            id: pairBtn
                            Layout.alignment: Qt.AlignHCenter
                            text: qsTr("配对")
                            highlighted: true
                            enabled: codeField.text.length >= 4 && !backend.devicePairing
                            onClicked: {
                                let service = ""
                                if (svcCombo.visible && svcCombo.currentIndex > 0
                                        && svcCombo.currentIndex <= page.pairingSvcs.length)
                                    service = page.pairingSvcs[svcCombo.currentIndex - 1].instance
                                backend.devicePairCode(codeField.text, manualAddrField.text, service)
                            }
                        }
                    }
                }
            }

            RowLayout {
                Layout.leftMargin: 24
                Layout.rightMargin: 24
                Layout.fillWidth: true
                spacing: 14

                // 方式三：USB 即插即用
                RGroupBox {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    title: qsTr("③ USB 数据线（即插即用）")
                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 10
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("插线即自动识别。首次需在手机上开启「USB 调试」并允许本机调试"
                                       + "（弹出授权框时勾选「一律允许」）。")
                            wrapMode: Text.WordWrap
                            font.pixelSize: 12
                            color: Theme.textFaint
                        }
                    }
                }

                // 方式四：手动连接（已配对设备 / adb tcpip 设备）
                RGroupBox {
                    Layout.fillWidth: true
                    Layout.preferredWidth: 1
                    title: qsTr("④ 手动连接（已配对设备）")
                    ColumnLayout {
                        anchors.fill: parent
                        spacing: 10
                        Text {
                            Layout.fillWidth: true
                            text: qsTr("已配对过的设备直接输「无线调试」主页显示的 IP:端口连接；"
                                       + "USB 连着的设备也可在下方列表一键转无线。")
                            wrapMode: Text.WordWrap
                            font.pixelSize: 12
                            color: Theme.textFaint
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            RTextField {
                                id: addrField
                                Layout.fillWidth: true
                                placeholderText: qsTr("192.168.1.42:40113")
                                font.pixelSize: 13
                                onAccepted: backend.deviceConnect(text)
                            }
                            RButton {
                                text: qsTr("连接")
                                highlighted: true
                                enabled: addrField.text.indexOf(":") > 0
                                onClicked: backend.deviceConnect(addrField.text)
                            }
                        }
                    }
                }
            }

            // ===== 设备列表 =====
            RGroupBox {
                Layout.leftMargin: 24
                Layout.rightMargin: 24
                Layout.fillWidth: true
                title: qsTr("设备")
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 8

                    // 空态引导
                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: backend.deviceList.length === 0
                        spacing: 6
                        Text {
                            Layout.topMargin: 8
                            Layout.alignment: Qt.AlignHCenter
                            text: qsTr("暂无真机在线")
                            font.pixelSize: 14
                            color: Theme.textFaint
                        }
                        Text {
                            Layout.alignment: Qt.AlignHCenter
                            text: qsTr("插上 USB 线，或用上方任一无线方式连接")
                            font.pixelSize: 12
                            color: Theme.textFaint
                        }
                    }

                    Repeater {
                        model: backend.deviceList
                        delegate: Rectangle {
                            required property var modelData
                            readonly property bool online: modelData.state === "device"
                            readonly property bool isWifi: modelData.transport === "wifi"
                            Layout.fillWidth: true
                            height: 64
                            radius: 10
                            color: Theme.dark ? "#1F1F23" : "#F4F4F6"

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 14
                                anchors.rightMargin: 12
                                spacing: 12

                                // 状态灯
                                Rectangle {
                                    width: 10; height: 10; radius: 5
                                    color: online ? Theme.success
                                         : modelData.state === "unauthorized" ? Theme.warning
                                         : Theme.textFaint
                                }

                                ColumnLayout {
                                    Layout.preferredWidth: 260
                                    spacing: 2
                                    Text {
                                        Layout.fillWidth: true
                                        text: modelData.model && modelData.model.length > 0
                                              ? modelData.model : modelData.serial
                                        font.pixelSize: 14; font.weight: Font.DemiBold
                                        color: Theme.text
                                        elide: Text.ElideRight
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: modelData.serial +
                                              (modelData.android && modelData.android.length > 0
                                                   ? qsTr(" · Android ") + modelData.android : "")
                                        font.pixelSize: 11
                                        color: Theme.textFaint
                                        elide: Text.ElideMiddle
                                    }
                                }

                                // 传输徽章
                                Rectangle {
                                    width: 44; height: 22; radius: 11
                                    color: isWifi ? Theme.accent : Theme.cardHover
                                    Text {
                                        anchors.centerIn: parent
                                        text: isWifi ? qsTr("Wi-Fi") : qsTr("USB")
                                        font.pixelSize: 11; font.weight: Font.Medium
                                        color: isWifi ? "white" : Theme.text
                                    }
                                }

                                Text {
                                    visible: modelData.state === "unauthorized"
                                    text: qsTr("手机上允许 USB 调试后生效")
                                    font.pixelSize: 11
                                    color: Theme.warning
                                }
                                Item { Layout.fillWidth: true }

                                RButton {
                                    text: qsTr("安装 APK")
                                    enabled: online
                                    onClicked: {
                                        apkDialog.targetSerial = modelData.serial
                                        apkDialog.open()
                                    }
                                }
                                RButton {
                                    text: qsTr("投屏")
                                    highlighted: true
                                    enabled: online
                                    onClicked: backend.deviceMirror(modelData.serial)
                                }
                                RButton {
                                    visible: online && !isWifi
                                    text: qsTr("转无线")
                                    onClicked: backend.deviceTcpip(modelData.serial)
                                }
                                RButton {
                                    visible: isWifi
                                    text: qsTr("断开")
                                    onClicked: backend.deviceDisconnect(modelData.serial)
                                }
                                RButton {
                                    text: qsTr("忘记")
                                    onClicked: backend.deviceForget(modelData.serial)
                                }
                            }
                        }
                    }

                    Text {
                        Layout.topMargin: 4
                        visible: backend.deviceList.length > 0
                        text: qsTr("已配对设备在同一 Wi-Fi 下会自动回连（首次需手动连接一次）")
                        font.pixelSize: 11
                        color: Theme.textFaint
                    }
                }
            }

            Item { Layout.fillHeight: true; Layout.bottomMargin: 12 }
        }
    }
}
