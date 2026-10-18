import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Houttuynia

// 真 adb shell 形态：终端区即输入区——按键直接转发给设备 shell（PTY 自带回显），
// 无本地输入框；方向键/Tab/Ctrl 组合按 xterm 序列发送（shell 历史与补全原生可用）。
Item {
    id: page

    // 页面可见 & VM 运行 & 未连接 → 自动连接
    function ensureConnected() {
        if (visible && backend.vmRunning && !backend.terminalConnected && backend.daemonOnline)
            backend.openTerminal()
    }
    Connections {
        target: backend
        function onTerminalChanged() { page.ensureConnected() }
        function onStatusChanged() { page.ensureConnected() }
    }
    onVisibleChanged: ensureConnected()

    // xterm 键序列映射
    function keySequence(event) {
        if (event.modifiers & Qt.ControlModifier) {
            const t = event.text
            if (t.length === 1) {
                const c = t.charCodeAt(0)
                if (c >= 64 && c < 96) // Ctrl+字母 → 控制字符（^A..\_）
                    return String.fromCharCode(c - 64)
            }
            return ""
        }
        switch (event.key) {
        case Qt.Key_Return:
        case Qt.Key_Enter: return "\r"
        case Qt.Key_Backspace: return "\x7f"
        case Qt.Key_Tab: return "\t"
        case Qt.Key_Up: return "\x1b[A"
        case Qt.Key_Down: return "\x1b[B"
        case Qt.Key_Right: return "\x1b[C"
        case Qt.Key_Left: return "\x1b[D"
        case Qt.Key_Home: return "\x1b[H"
        case Qt.Key_End: return "\x1b[F"
        case Qt.Key_PageUp: return "\x1b[5~"
        case Qt.Key_PageDown: return "\x1b[6~"
        case Qt.Key_Delete: return "\x1b[3~"
        case Qt.Key_Insert: return "\x1b[2~"
        case Qt.Key_Escape: return "\x1b"
        }
        return event.text // 可打印字符（含中文，IME 确认后到达）
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 24
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            Text {
                text: qsTr("ADB 终端")
                font.pixelSize: 22; font.weight: Font.Bold
                color: Theme.text
            }
            Item { Layout.fillWidth: true }
            Rectangle {
                width: 10; height: 10; radius: 5
                color: backend.terminalConnected ? Theme.success : Theme.textFaint
            }
            Text {
                text: backend.terminalConnected ? qsTr("已连接") : qsTr("未连接")
                font.pixelSize: 12; color: Theme.textDim
            }
            RButton { text: qsTr("清屏"); onClicked: backend.terminalClear() }
            RButton {
                text: backend.terminalConnected ? qsTr("断开重连") : qsTr("连接")
                onClicked: {
                    if (backend.terminalConnected)
                        backend.closeTerminal()
                    backend.openTerminal()
                }
            }
        }

        // 终端视图：全区域可打字（非只读，但拦截一切按键转发 shell，本地不落字）
        Rectangle {
            Layout.fillWidth: true
            Layout.fillHeight: true
            radius: Theme.radius
            color: Theme.termBg
            border.color: backend.terminalConnected ? Theme.separator : Theme.warnBorder

            ScrollView {
                id: termScroll
                anchors.fill: parent
                anchors.margins: 8

                TextArea {
                    id: termView
                    // 只读显示：回显文本绝不能走输入路径（可编辑态下输出更新会被
                    // 输入法/编辑管线当作用户输入反馈回 shell，形成无限回显循环）
                    readOnly: true
                    wrapMode: TextArea.WrapAnywhere
                    text: backend.terminalOutput
                    font.family: Theme.mono
                    font.pixelSize: 13
                    color: "#E4E4E8"
                    selectionColor: Theme.accent
                    background: null
                    cursorVisible: true

                    // 光标钉在末尾（新输出自动滚动）
                    onTextChanged: cursorPosition = text.length
                    onCursorRectangleChanged: {
                        const bar = termScroll.ScrollBar.vertical
                        if (bar.size === 0 || bar.position > 0.85)
                            bar.position = 1 - bar.size
                    }

                    // 按键全拦截 → 转 xterm 序列发给 shell
                    Keys.onPressed: (event) => {
                        if (!backend.terminalConnected) {
                            event.accepted = false
                            return
                        }
                        const seq = page.keySequence(event)
                        if (seq.length > 0) {
                            backend.terminalWrite(seq)
                            event.accepted = true
                        } else {
                            event.accepted = false
                        }
                    }
                    // 选中复制等 Cmd 快捷键放行（macOS 上 Cmd 不走 Control 分支）
                }

                TapHandler {
                    onTapped: termView.forceActiveFocus()
                }
            }

            // 未连接遮罩提示
            ColumnLayout {
                anchors.centerIn: parent
                visible: !backend.terminalConnected
                spacing: 8
                Text { text: "⌨️"; font.pixelSize: 34; Layout.alignment: Qt.AlignHCenter }
                Text {
                    text: backend.vmRunning ? qsTr("点击连接 ADB 终端")
                                            : qsTr("Android 未运行——启动后自动连接")
                    font.pixelSize: 12; color: Theme.textDim
                    Layout.alignment: Qt.AlignHCenter
                }
                RButton {
                    visible: backend.vmRunning && !backend.terminalConnected
                    text: qsTr("连接")
                    highlighted: true
                    onClicked: backend.openTerminal()
                }
            }
        }

        Text {
            font.pixelSize: 11
            color: Theme.textFaint
            text: qsTr("直接输入命令 · ↑↓ 历史 · Tab 补全 · Ctrl-C 中断 · Ctrl-D 退出 · Ctrl-L 清屏")
        }
    }
}
