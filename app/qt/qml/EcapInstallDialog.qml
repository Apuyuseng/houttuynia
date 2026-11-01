import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Houttuynia

// ecapture 按需下载引导弹窗（app 不内置组件；设置页开启开关 / 流量分析页
// 开始采集时组件缺失弹出）。autoStart：下载完成后自动开始采集（采集页场景）。
Dialog {
    id: root

    property bool autoStart: false
    // 本实例发起的下载才响应完成信号（captureBinInstalled 是广播：
    // 采集页 autoStart 实例不能替设置页发起的下载自动开采集）
    property bool downloadRequested: false

    modal: true
    anchors.centerIn: parent
    title: qsTr("安装流量分析组件")
    width: 440
    // 下载中隐藏标准按钮（进度与取消在内容区）
    standardButtons: backend.captureBinStatus.installing === true
                     ? Dialog.NoButton : (Dialog.Cancel | Dialog.Ok)

    ColumnLayout {
        spacing: 10
        width: parent ? parent.width - 20 : 420

        Label {
            wrapMode: Text.WordWrap
            color: Theme.text
            text: qsTr("流量分析需要 ecapture 组件（gojue/ecapture，Apache-2.0，压缩后约 13MB）。"
                       + "它不随应用内置：首次使用时从 GitHub Releases 下载并安装到 "
                       + "~/.mwine/bin/ecapture，支持断点续传。")
        }
        Label {
            visible: (backend.captureBinTask.error || "").length > 0
            wrapMode: Text.WordWrap
            color: Theme.danger
            text: backend.captureBinTask.error || ""
        }
        RowLayout {
            visible: backend.captureBinStatus.installing === true
            spacing: 10
            Layout.fillWidth: true
            ProgressBar {
                Layout.fillWidth: true
                from: 0
                to: backend.captureBinTask.progress_total || 1
                value: backend.captureBinTask.progress || 0
            }
            Text {
                visible: (backend.captureBinTask.progress_total || 0) > 0
                text: Math.floor((backend.captureBinTask.progress || 0) * 100
                                 / (backend.captureBinTask.progress_total || 1)) + "%"
                font.pixelSize: 12; color: Theme.textDim
            }
            RButton {
                text: qsTr("取消")
                onClicked: backend.captureBinCancel()
            }
        }
    }
    onAccepted: {
        downloadRequested = true
        backend.captureBinInstall()
    }

    Connections {
        target: backend
        // 组件落位：仅本实例发起的下载——关弹窗；采集页场景（autoStart）接着开始采集
        function onCaptureBinInstalled() {
            if (!downloadRequested)
                return
            downloadRequested = false
            root.close()
            if (root.autoStart && backend.vmRunning)
                backend.captureStart()
        }
    }
}
