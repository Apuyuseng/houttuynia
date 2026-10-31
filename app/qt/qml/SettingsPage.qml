import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Houttuynia

Item {
    id: page

    // 页面可见时刷新 ecapture 组件状态（"已就绪"提示与开关引导依赖它；
    // 组件状态平时只在流量分析页轮询，从设置页开启功能前可能从未拉取）
    onVisibleChanged: {
        if (visible && backend.captureEnabled)
            backend.refreshCaptureBinStatus()
    }

    property var cfg: backend.config
    // 编辑状态（保存时一次性提交）
    property string mode: cfg.mode || "phone"
    property int custW: cfg.width || 1080
    property int custH: cfg.height || 2400
    property int dpi: cfg.dpi || 440
    property string locale: cfg.locale || ""
    property int memoryMB: cfg.memory_mb || 4096
    property int vsyncRate: cfg.vsync_rate || 0
    property string cameraBack: cfg.camera_back || ""
    property string cameraFront: cfg.camera_front || ""
    property bool hostAudio: cfg.allow_host_audio || false
    property string proxy: cfg.proxy || ""
    property string netspeed: cfg.netspeed || ""
    property bool noSnapshot: cfg.no_snapshot || false
    property bool noBootAnim: cfg.no_boot_anim || false
    property bool writableSystem: cfg.writable_system || false
    property string qemuExtra: cfg.qemu_extra || ""
    property int idleStopMin: cfg.idle_stop_min || 30
    property bool hideEmuWin: cfg.hide_emulator_window || false
    property bool windowAll: cfg.window_mode_all || false
    property bool windowBorderless: cfg.window_borderless || false

    onCfgChanged: syncTimer.restart()   // 配置到达/刷新 → 控件回填（延迟到控件就绪）

    // 控件回填（服务端值 → 控件索引/文本）
    function syncFromConfig() {
        const modeVals = ["phone", "tablet", "tv", "custom"]
        modeBox.currentIndex = Math.max(0, modeVals.indexOf(mode))
        const localeVals = ["", "zh-CN", "en-US", "ja-JP", "ko-KR"]
        localeBox.currentIndex = Math.max(0, localeVals.indexOf(locale))
        wSpin.value = custW; hSpin.value = custH; dpiSpin.value = dpi
        memSlider.value = memoryMB
        const vsVals = [0, 60, 90, 120]
        vsyncBox.currentIndex = Math.max(0, vsVals.indexOf(vsyncRate))
        const cbVals = ["", "none", "emulated", "virtualscene", "webcam0"]
        camBackBox.currentIndex = Math.max(0, cbVals.indexOf(cameraBack))
        const cfVals = ["", "none", "emulated", "webcam0"]
        camFrontBox.currentIndex = Math.max(0, cfVals.indexOf(cameraFront))
        hostAudioBox.checked = hostAudio
        proxyField.text = proxy
        const nsVals = ["", "lte", "umts", "edge", "gsm"]
        netspeedBox.currentIndex = Math.max(0, nsVals.indexOf(netspeed))
        noSnapshotBox.checked = noSnapshot
        noBootAnimBox.checked = noBootAnim
        writableSystemBox.checked = writableSystem
        qemuExtraField.text = qemuExtra
        idleSpin.value = idleStopMin
        hideEmuWinBox.checked = hideEmuWin
        windowAllBox.checked = windowAll
        borderlessBox.checked = windowBorderless
    }
    Timer { id: syncTimer; interval: 30; onTriggered: page.syncFromConfig() }
    Component.onCompleted: page.syncFromConfig()

    function doSave() {
        // 必须基于完整配置合并（daemon 反序列化要求 sdcard_path/avd 等无默认值字段齐全）
        const out = {}
        for (const k in cfg) out[k] = cfg[k]
        out.mode = mode; out.width = custW; out.height = custH; out.dpi = dpi
        out.locale = locale; out.memory_mb = memoryMB; out.vsync_rate = vsyncRate
        out.camera_back = cameraBack; out.camera_front = cameraFront
        out.allow_host_audio = hostAudio; out.proxy = proxy; out.netspeed = netspeed
        out.no_snapshot = noSnapshot; out.no_boot_anim = noBootAnim
        out.writable_system = writableSystem; out.qemu_extra = qemuExtra
        out.idle_stop_min = idleStopMin
        out.hide_emulator_window = hideEmuWin
        out.window_mode_all = windowAll
        out.window_borderless = windowBorderless
        backend.saveConfig(out)
    }

    // 区块标题（GroupBox 内容的第一项）
    component SectionLabel: ColumnLayout {
        property string icon: ""
        property string title: ""
        property string subtitle: ""
        spacing: 1
        Layout.fillWidth: true
        Text {
            text: parent.icon + " " + parent.title
            font.pixelSize: 14; font.weight: Font.Bold; color: Theme.text
        }
        Text {
            visible: parent.subtitle.length > 0
            text: parent.subtitle
            font.pixelSize: 11; color: Theme.textDim
        }
    }

    component FieldLabel: Label {
        font.pixelSize: 12
        color: Theme.text
    }

    // 通用表单网格（GroupBox 内使用）
    component FormGrid: GridLayout {
        columns: 2
        columnSpacing: 16; rowSpacing: 10
        Layout.fillWidth: true
    }

    // 通用说明行
    component Hint: Text {
        font.pixelSize: 11; color: Theme.textDim
        wrapMode: Text.WordWrap
        Layout.fillWidth: true
    }

    ScrollView {
        anchors.fill: parent
        contentWidth: availableWidth

        ColumnLayout {
            width: Math.min(parent.width - 48, 720)
            x: (parent.width - width) / 2
            spacing: 14

            RowLayout {
                Layout.fillWidth: true
                Text {
                    text: qsTr("设置")
                    font.pixelSize: 22; font.weight: Font.Bold
                    color: Theme.text
                }
                Item { Layout.fillWidth: true }
                RButton { text: qsTr("保存"); highlighted: true; onClicked: page.doSave() }
            }

            // ===== 语言 / Language =====
            RGroupBox {
                Layout.fillWidth: true
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 10
                    SectionLabel { icon: "🌍"; title: qsTr("语言"); subtitle: qsTr("界面语言，即时生效；后续可扩展更多语言") }

                    FormGrid {
                        FieldLabel { text: qsTr("界面语言") }
                        RComboBox {
                            id: langBox
                            Layout.fillWidth: true
                            // 与 backend.availableLanguages() 对齐（当前内置中文/英文）
                            property var codes: ["zh", "en"]
                            model: [qsTr("简体中文"), "English"]
                            Component.onCompleted: currentIndex = Math.max(0, codes.indexOf(backend.language))
                            onActivated: function(index) { backend.setLanguage(codes[index]) }
                        }
                    }
                }
            }

            // ===== 设备形态 =====
            RGroupBox {
                Layout.fillWidth: true
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 10
                    SectionLabel { icon: "📱"; title: qsTr("设备形态"); subtitle: qsTr("虚拟机的外观与屏幕规格") }

                    FormGrid {
                        FieldLabel { text: qsTr("模式") }
                        RComboBox {
                            id: modeBox
                            Layout.fillWidth: true
                            model: [qsTr("📱 手机（竖屏）"), qsTr("🖥 平板（横屏）"), qsTr("📺 TV"), qsTr("⚙️ 自定义")]
                            onCurrentIndexChanged: {
                                const vals = ["phone", "tablet", "tv", "custom"]
                                page.mode = vals[currentIndex]
                                customSize.visible = page.mode === "custom"
                            }
                        }

                        FieldLabel { text: qsTr("系统语言") }
                        RComboBox {
                            id: localeBox
                            Layout.fillWidth: true
                            model: [qsTr("跟随电脑"), qsTr("简体中文"), "English", qsTr("日本語"), qsTr("한국어")]
                            onCurrentIndexChanged: {
                                const vals = ["", "zh-CN", "en-US", "ja-JP", "ko-KR"]
                                page.locale = vals[currentIndex]
                            }
                        }

                        FieldLabel { text: qsTr("独立窗口模式") }
                        RCheckBox {
                            id: windowAllBox
                            text: qsTr("所有应用使用独立窗口（模拟器主窗自动隐藏）")
                            font.pixelSize: 12
                            onToggled: page.windowAll = checked
                        }

                        FieldLabel { text: qsTr("模拟器主窗") }
                        RCheckBox {
                            id: hideEmuWinBox
                            text: qsTr("独立窗口模式时隐藏（启动主屏应用时自动恢复）")
                            font.pixelSize: 12
                            enabled: !windowAllBox.checked
                            onToggled: page.hideEmuWin = checked
                        }

                        FieldLabel { text: qsTr("独立窗口边框") }
                        RCheckBox {
                            id: borderlessBox
                            text: qsTr("无边框（⌘拖动移动、⌥拖动缩放、Esc 返回、⌘W 关闭）")
                            font.pixelSize: 12
                            onToggled: page.windowBorderless = checked
                        }
                    }

                    RowLayout {
                        id: customSize
                        visible: false
                        Layout.fillWidth: true
                        spacing: 8
                        FieldLabel { text: "尺寸" }
                        RSpinBox { id: wSpin; from: 320; to: 4096; editable: true
                                  onValueChanged: page.custW = value }
                        Label { text: "×"; color: Theme.textDim }
                        RSpinBox { id: hSpin; from: 480; to: 4096; editable: true
                                  onValueChanged: page.custH = value }
                        Label { text: "@"; color: Theme.textDim }
                        RSpinBox { id: dpiSpin; from: 120; to: 640; editable: true
                                  onValueChanged: page.dpi = value }
                        Label { text: "DPI"; color: Theme.textDim }
                        Item { Layout.fillWidth: true }
                    }

                    Hint { text: qsTr("运行中保存 → 屏幕立即切换；分辨率重启完全生效。") }
                    Hint { text: qsTr("独立窗口模式重启虚拟机后生效；开启后主屏应用不可见，主屏画面可用镜像窗口查看。") }
                }
            }

            // ===== 性能 =====
            RGroupBox {
                Layout.fillWidth: true
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 10
                    SectionLabel { icon: "⚡"; title: qsTr("性能"); subtitle: qsTr("内存与帧率——影响流畅度与资源占用") }

                    FormGrid {
                        FieldLabel { text: qsTr("内存") }
                        RowLayout {
                            spacing: 8
                            Layout.fillWidth: true
                            RSlider {
                                id: memSlider
                                from: 1024; to: 8192; stepSize: 256
                                onValueChanged: {
                                    page.memoryMB = value
                                    memLabel.text = value + " MB"
                                }
                                Layout.preferredWidth: 200
                            }
                            Label { id: memLabel; font.pixelSize: 12; color: Theme.textDim }
                        }

                        FieldLabel { text: qsTr("屏幕刷新率") }
                        RComboBox {
                            id: vsyncBox
                            Layout.fillWidth: true
                            model: [qsTr("默认 60Hz"), "60 Hz", qsTr("90 Hz（部分游戏高帧）"), qsTr("120 Hz（高端机）")]
                            onCurrentIndexChanged: page.vsyncRate = [0, 60, 90, 120][currentIndex]
                        }
                    }

                    Hint { text: qsTr("建议 4GB 起步；游戏多开选 6-8GB。90/120Hz 可解锁高帧率。重启生效。") }
                }
            }

            // ===== 摄像头与麦克风 =====
            RGroupBox {
                Layout.fillWidth: true
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 10
                    SectionLabel { icon: "📷"; title: qsTr("摄像头与麦克风"); subtitle: qsTr("虚拟设备如何使用宿主机硬件") }

                    FormGrid {
                        FieldLabel { text: qsTr("后置摄像头") }
                        RComboBox {
                            id: camBackBox
                            Layout.fillWidth: true
                            model: [qsTr("默认"), qsTr("🚫 关闭"), qsTr("🎞 模拟画面"), qsTr("🎬 虚拟场景"), qsTr("💻 宿主机摄像头")]
                            onCurrentIndexChanged: page.cameraBack = ["", "none", "emulated", "virtualscene", "webcam0"][currentIndex]
                        }

                        FieldLabel { text: qsTr("前置摄像头") }
                        RComboBox {
                            id: camFrontBox
                            Layout.fillWidth: true
                            model: [qsTr("默认"), qsTr("🚫 关闭"), qsTr("🎞 模拟画面"), qsTr("💻 宿主机摄像头")]
                            onCurrentIndexChanged: page.cameraFront = ["", "none", "emulated", "webcam0"][currentIndex]
                        }

                        FieldLabel { text: qsTr("麦克风") }
                        RCheckBox {
                            id: hostAudioBox
                            text: qsTr("允许应用使用宿主机麦克风")
                            font.pixelSize: 12
                            onToggled: page.hostAudio = checked
                        }
                    }
                }
            }

            // ===== 网络 =====
            RGroupBox {
                Layout.fillWidth: true
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 10
                    SectionLabel { icon: "🌐"; title: qsTr("网络"); subtitle: qsTr("代理与网速模拟") }

                    FormGrid {
                        FieldLabel { text: qsTr("HTTP 代理") }
                        RTextField {
                            id: proxyField
                            placeholderText: qsTr("http://127.0.0.1:7890（留空不代理）")
                            onTextChanged: page.proxy = text
                            Layout.fillWidth: true
                            font.family: Theme.mono
                            font.pixelSize: 12
                        }

                        FieldLabel { text: qsTr("网速模拟") }
                        RComboBox {
                            id: netspeedBox
                            Layout.fillWidth: true
                            model: [qsTr("不限速"), qsTr("📶 4G"), qsTr("📶 3G"), qsTr("📶 2G"), qsTr("✈️ 极慢")]
                            onCurrentIndexChanged: page.netspeed = ["", "lte", "umts", "edge", "gsm"][currentIndex]
                        }
                    }
                }
            }

            // ===== 存储 / 快照 =====
            RGroupBox {
                Layout.fillWidth: true
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 10
                    SectionLabel { icon: "💾"; title: qsTr("存储与快照"); subtitle: qsTr("磁盘容量与命名存档点") }

                    FormGrid {
                        FieldLabel { text: qsTr("磁盘容量") }
                        RowLayout {
                            spacing: 8
                            Layout.fillWidth: true
                            Text {
                                text: backend.diskGB + " GB"
                                font.pixelSize: 12; color: Theme.text
                            }
                            RButton {
                                text: qsTr("扩容…")
                                onClicked: {
                                    diskDialog.open()
                                    diskSpin.value = backend.diskGB
                                }
                            }
                            Item { Layout.fillWidth: true }
                        }

                        FieldLabel { text: qsTr("命名存档") }
                        RowLayout {
                            spacing: 6
                            Layout.fillWidth: true
                            RTextField {
                                id: snapName
                                placeholderText: qsTr("存档名（字母/数字/_/-）")
                                Layout.preferredWidth: 170
                                font.pixelSize: 12
                            }
                            RButton {
                                text: qsTr("保存存档点")
                                enabled: backend.vmRunning && snapName.text.length > 0
                                onClicked: backend.snapshotSave(snapName.text)
                            }
                        }
                    }

                    // 存档点列表
                    Flow {
                        visible: backend.snapshots.length > 0
                        Layout.fillWidth: true
                        spacing: 6

                        Repeater {
                            model: backend.snapshots
                            delegate: Rectangle {
                                readonly property var snap: modelData
                                width: snapRow.implicitWidth + 20
                                height: 32
                                radius: 16
                                color: Theme.cardHover
                                border.color: Theme.separator

                                RowLayout {
                                    id: snapRow
                                    anchors.centerIn: parent
                                    spacing: 8
                                    Text {
                                        text: "💾 " + snap.name
                                        font.pixelSize: 12; color: Theme.text
                                    }
                                    ToolButton {
                                        text: "▶"
                                        font.pixelSize: 11
                                        ToolTip.visible: hovered; ToolTip.text: qsTr("恢复此存档")
                                        onClicked: backend.snapshotLoad(snap.name)
                                    }
                                    ToolButton {
                                        text: "✕"
                                        font.pixelSize: 11
                                        ToolTip.visible: hovered; ToolTip.text: qsTr("删除存档")
                                        onClicked: backend.snapshotDelete(snap.name)
                                    }
                                }
                            }
                        }
                    }

                    Text {
                        visible: backend.snapshots.length === 0
                        text: qsTr("暂无命名存档点（关机自动快照由系统管理，不受此影响）")
                        font.pixelSize: 11; color: Theme.textDim
                    }
                }
            }

            // ===== 高级选项 =====
            RGroupBox {
                Layout.fillWidth: true
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 10
                    SectionLabel { icon: "🔬"; title: qsTr("高级选项"); subtitle: qsTr("面向有经验的用户，一般无需修改") }

                    FormGrid {
                        FieldLabel { text: qsTr("快照") }
                        RCheckBox { id: noSnapshotBox; text: qsTr("禁用（每次冷启动）"); font.pixelSize: 12
                                    onToggled: page.noSnapshot = checked }

                        FieldLabel { text: qsTr("开机动画") }
                        RCheckBox { id: noBootAnimBox; text: qsTr("跳过"); font.pixelSize: 12
                                    onToggled: page.noBootAnim = checked }

                        FieldLabel { text: qsTr("系统分区") }
                        RCheckBox { id: writableSystemBox; text: qsTr("允许修改"); font.pixelSize: 12
                                    onToggled: page.writableSystem = checked }

                        FieldLabel { text: qsTr("空闲关机") }
                        RSpinBox {
                            id: idleSpin
                            from: 0; to: 240; editable: true
                            onValueChanged: page.idleStopMin = value
                            textFromValue: function(v) { return v === 0 ? "不自动" : v + " 分钟" }
                            valueFromText: function(t) { return t === "不自动" ? 0 : parseInt(t) }
                        }

                        FieldLabel { text: qsTr("QEMU 参数") }
                        RTextField {
                            id: qemuExtraField
                            placeholderText: qsTr("-show-kernel")
                            onTextChanged: page.qemuExtra = text
                            Layout.fillWidth: true
                            font.family: Theme.mono
                            font.pixelSize: 12
                        }
                    }
                }
            }

            // ===== 功能开关 =====
            RGroupBox {
                Layout.fillWidth: true
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 10
                    SectionLabel { icon: "📡"; title: qsTr("功能开关"); subtitle: qsTr("按需启用的进阶功能") }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12
                        ColumnLayout {
                            spacing: 1
                            FieldLabel { text: qsTr("流量分析（eBPF 明文旁挂）") }
                            Text {
                                text: qsTr("开启后侧栏显示入口；ecapture 组件（约 13MB）首次开启时下载，"
                                           + "需 userdebug 镜像")
                                font.pixelSize: 11; color: Theme.textDim
                                wrapMode: Text.WordWrap
                                Layout.fillWidth: true
                            }
                        }
                        Item { Layout.fillWidth: true }
                        RCheckBox {
                            id: captureToggle
                            text: qsTr("启用")
                            font.pixelSize: 12
                            checked: backend.captureEnabled
                            onToggled: backend.setCaptureEnabled(checked)
                        }
                    }

                    Text {
                        visible: backend.captureBinStatus.present === true
                        text: qsTr("✓ ecapture 组件已就绪：") + (backend.captureBinStatus.path || "")
                        font.pixelSize: 11; color: Theme.textFaint
                        elide: Text.ElideMiddle
                        Layout.fillWidth: true
                    }
                }
            }

            // ===== 危险区 =====
            RGroupBox {
                Layout.fillWidth: true
                ColumnLayout {
                    anchors.fill: parent
                    spacing: 10
                    SectionLabel { icon: "🛡"; title: qsTr("设备身份与重置") }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12
                        ColumnLayout {
                            spacing: 1
                            FieldLabel { text: qsTr("设备身份重置") }
                            Text {
                                text: qsTr("android_id / 广告 ID 换新，应用视作全新设备")
                                font.pixelSize: 11; color: Theme.textDim
                            }
                        }
                        Item { Layout.fillWidth: true }
                        RButton { text: qsTr("重置身份"); onClicked: backend.identityReset() }
                    }

                    Rectangle { Layout.fillWidth: true; height: 1; color: Theme.separator }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12
                        ColumnLayout {
                            spacing: 1
                            Text {
                                text: qsTr("重置虚拟机")
                                font.pixelSize: 12; color: Theme.danger
                                font.weight: Font.DemiBold
                            }
                            Text {
                                text: qsTr("清空全部应用、数据与快照，下次启动为全新系统")
                                font.pixelSize: 11; color: Theme.textDim
                            }
                        }
                        Item { Layout.fillWidth: true }
                        RButton { text: qsTr("重置虚拟机…"); onClicked: vmResetDialog.open() }
                    }
                }
            }

            Item { height: 24 }
        }
    }

    // 磁盘扩容确认
    Dialog {
        id: diskDialog
        modal: true
        anchors.centerIn: parent
        title: qsTr("调整磁盘容量")
        standardButtons: Dialog.Cancel | Dialog.Ok
        width: 400

        ColumnLayout {
            spacing: 10
            width: parent ? parent.width : 360
            Label {
                wrapMode: Text.WordWrap
                color: Theme.text
                text: qsTr("扩容/缩容会重建数据磁盘：清空全部应用与数据并关停虚拟机。建议先保存命名存档点（存档不受影响）。")
            }
            RowLayout {
                spacing: 8
                Label { text: qsTr("新容量（GB）") }
                RSpinBox { id: diskSpin; from: 4; to: 512; editable: true }
            }
        }
        onAccepted: backend.resizeDisk(diskSpin.value)
    }

    // VM 重置确认
    Dialog {
        id: vmResetDialog
        modal: true
        anchors.centerIn: parent
        title: qsTr("重置虚拟机")
        standardButtons: Dialog.Cancel | Dialog.Yes
        width: 400

        Label {
            width: parent ? parent.width : 360
            wrapMode: Text.WordWrap
            color: Theme.text
            text: qsTr("将关停虚拟机并清空 userdata 与全部快照，所有应用与数据将丢失且不可恢复。确定继续？")
        }
        onAccepted: backend.vmReset()
    }

    // 开启流量分析但组件缺失：下载引导（完成后组件就绪提示自动刷新）
    EcapInstallDialog { id: ecapDialog; autoStart: false }

    Connections {
        target: backend
        function onCaptureBinMissing() { ecapDialog.open() }
    }
}
