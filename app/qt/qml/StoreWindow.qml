import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Effects
import Houttuynia

// 在线下载独立窗口（App Store 风格）：像普通应用一样弹出
// （应用库网格的「在线下载」入口与概览页快捷方式共用 Main.openStoreWindow() 打开）。
// 关闭仅隐藏——保留搜索结果与下载任务状态，再次点击图标重新前置。
//
// 落地页：分段来源选择 + 居中搜索；主推大卡（Editorial）+ 今日推荐横排 +
// 更多精选网格；选中任一应用，底部滑出版本面板（覆盖右下半屏）。
// 图标源：F-Droid 各镜像均不携带图标文件（实测 TUNA/NJU/USTC/官方全 404），
// 故用 App Store 同款连续圆角（squircle 近似）渐变头像，iconUrl 可达时仍优先真图标。
ApplicationWindow {
    id: storeWin

    title: qsTr("在线下载")
    width: 1080; height: 700
    minimumWidth: 900; minimumHeight: 600
    color: Theme.window
    visible: false

    onClosing: function(close) {
        backend.saveWinGeometry("store", x, y, width, height)
        close.accepted = false
        storeWin.hide()
    }

    property string selectedPkg: ""

    // 搜索模式 = 搜索框有词或正在搜索
    readonly property bool searchMode: searchBox.text.length > 0 || backend.storeSearching

    // 已安装包集合（安装徽标）
    property var installedSet: {
        const s = {}
        for (let i = 0; i < backend.apps.length; i++)
            s[backend.apps[i].package] = true
        return s
    }

    // F-Droid 镜像列表（/api/store/sources）
    property var fdroidMirrors: []
    Connections {
        target: backend
        function onStoreChanged() {
            if (storeWin.fdroidMirrors.length || backend.storeSources.length === 0)
                return
            for (let i = 0; i < backend.storeSources.length; i++) {
                const s = backend.storeSources[i]
                if (s.id === "fdroid") {
                    storeWin.fdroidMirrors = s.mirrors || []
                    const names = [qsTr("自动（国内优先）")]
                    for (const m of storeWin.fdroidMirrors) names.push(m.name)
                    mirrorBox.model = names
                }
            }
        }
    }

    Component.onCompleted: {
        const geo = backend.winGeometry("store")
        if (geo.length === 4 && geo[0] >= 0 && geo[1] >= 0
                && geo[2] >= minimumWidth && geo[3] >= minimumHeight) {
            x = geo[0]; y = geo[1]; width = geo[2]; height = geo[3]
        }
        if (backend.storeRecommended.length === 0
                && !backend.storeRecommendLoading
                && backend.storeRecommendError.length === 0)
            storeWin.reloadRecommend()
    }

    function currentSource() {
        return ["", "wandoujia", "fdroid"][sourceSeg.currentIndex] || ""
    }
    function currentMirror() {
        return mirrorBox.currentIndex > 0 && storeWin.fdroidMirrors[mirrorBox.currentIndex - 1]
                ? storeWin.fdroidMirrors[mirrorBox.currentIndex - 1].id : ""
    }
    function reloadRecommend() {
        backend.storeRecommend("fdroid", storeWin.currentMirror())
    }
    function fmtSize(bytes) {
        if (!bytes) return ""
        return bytes >= 1073741824 ? (bytes / 1073741824).toFixed(2) + " GB"
             : (bytes / 1048576).toFixed(1) + " MB"
    }
    function doSearch() {
        const src = storeWin.currentSource()
        const mir = src === "fdroid" ? storeWin.currentMirror() : ""
        backend.storeSearch(searchBox.text, src, mir)
    }
    function openApp(app) {
        storeWin.selectedPkg = app.package
        backend.storeLoadVersions(app)
        versionSheet.open()
    }

    // ================= 连续圆角（squircle 近似）渐变头像 =================
    // App Store 风格：图标 = 圆角矩形渐变底 + 白色首字母；iconUrl 可达时优先真图标
    component AppAvatar: Item {
        id: avatar
        property var app: ({})
        property int pixelSize: 22
        property int radius: 12

        width: 48; height: 48

        Rectangle {
            id: avatarBg
            anchors.fill: parent
            radius: avatar.radius
            gradient: Gradient {
                GradientStop {
                    position: 0
                    color: Qt.lighter(Theme.avatarColor((app && app.package) || "?"), 1.32)
                }
                GradientStop {
                    position: 1
                    color: Qt.darker(Theme.avatarColor((app && app.package) || "?"), 1.24)
                }
            }
        }
        Image {
            id: avatarIcon
            anchors.fill: parent
            anchors.margins: 1
            source: (app && app.iconUrl && String(app.iconUrl).length > 0) ? app.iconUrl : ""
            fillMode: Image.PreserveAspectCrop
            smooth: true
            visible: false   // 真图标经 MultiEffect(mask) 渲染，原 Image 只作 source 不直接显示
        }
        // 真图标圆角裁剪：用 MultiEffect（Qt6 原生）的 mask，避免 OpacityMask
        // （Qt5Compat.GraphicalEffects，本环境不可用）依赖
        MultiEffect {
            anchors.fill: avatarIcon
            source: avatarIcon
            visible: avatarIcon.status === Image.Ready
            maskEnabled: true
            maskSource: avatarMask
            maskThresholdMin: 0.5
            maskSpreadAtMin: 1.0
        }
        Item {
            id: avatarMask
            width: avatarIcon.width; height: avatarIcon.height
            visible: false
            layer.enabled: true
            Rectangle {
                anchors.fill: parent
                radius: avatar.radius
            }
        }
        Text {
            anchors.centerIn: parent
            visible: avatarIcon.status !== Image.Ready
            text: ((app && (app.name || app.package)) || "?").charAt(0).toUpperCase()
            color: "white"; font.pixelSize: avatar.pixelSize
            font.weight: Font.DemiBold
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ============ 顶部条：来源分段 + 居中搜索 + 右侧开关 ============
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: 64

            // 来源分段（左）
            Item {
                anchors.left: parent.left
                anchors.leftMargin: 20
                anchors.verticalCenter: parent.verticalCenter
                width: segRow.implicitWidth
                height: 30

                Rectangle {
                    anchors.fill: parent
                    radius: 8
                    color: Theme.dark ? Qt.rgba(1,1,1,0.08) : Qt.rgba(0,0,0,0.06)
                }
                Row {
                    id: segRow
                    anchors.centerIn: parent
                    padding: 3
                    spacing: 3
                    Repeater {
                        model: [qsTr("全部"), qsTr("豌豆荚"), "F-Droid"]
                        delegate: Rectangle {
                            id: segBtn
                            readonly property bool active: sourceSeg.currentIndex === index
                            width: segLabel.implicitWidth + 22
                            height: 24
                            radius: 6
                            color: active ? Theme.card : "transparent"
                            border.width: active ? 1 : 0
                            border.color: Theme.separator
                            Text {
                                id: segLabel
                                anchors.centerIn: parent
                                text: modelData
                                font.pixelSize: 12
                                font.weight: active ? Font.DemiBold : Font.Normal
                                color: active ? Theme.text : Theme.textDim
                            }
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: {
                                    sourceSeg.currentIndex = index
                                    mirrorRow.visible = sourceSeg.currentIndex === 2
                                    if (!storeWin.searchMode && sourceSeg.currentIndex === 2
                                            && backend.storeRecommended.length === 0)
                                        storeWin.reloadRecommend()
                                }
                            }
                        }
                    }
                }
            }
            // 隐藏的对象占位（分段索引）
            QtObject { id: sourceSeg; property int currentIndex: 2 }

            // 居中搜索（App Store 风格：圆角搜索胶囊）
            Rectangle {
                anchors.centerIn: parent
                width: Math.min(460, parent.width * 0.46)
                height: 34
                radius: 17
                color: Theme.dark ? Qt.rgba(1,1,1,0.10) : Qt.rgba(0,0,0,0.06)

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 8
                    spacing: 6

                    Text { text: "🔍"; font.pixelSize: 13; opacity: 0.5 }
                    TextInput {
                        id: searchBox
                        Layout.fillWidth: true
                        font.pixelSize: 13
                        color: Theme.text
                        clip: true
                        selectByMouse: true
                        verticalAlignment: Text.AlignVCenter
                        onAccepted: storeWin.doSearch()
                        Text {
                            anchors.fill: parent
                            verticalAlignment: Text.AlignVCenter
                            visible: !searchBox.text && !searchBox.activeFocus
                            text: qsTr("搜索应用、游戏…")
                            font.pixelSize: 13
                            color: Theme.textFaint
                        }
                    }
                    Text {
                        visible: searchBox.text.length > 0
                        text: "✕"
                        font.pixelSize: 11
                        color: Theme.textFaint
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: searchBox.text = ""
                        }
                    }
                }
            }

            // 右侧：保留安装包开关（小）
            Row {
                anchors.right: parent.right
                anchors.rightMargin: 20
                anchors.verticalCenter: parent.verticalCenter
                spacing: 6
                RCheckBox {
                    text: qsTr("保留安装包")
                    checked: backend.keepApk
                    onToggled: backend.keepApk = checked
                    font.pixelSize: 12
                }
            }
        }

        // F-Droid 镜像选择行（仅 F-Droid 源显示）
        RowLayout {
            id: mirrorRow
            visible: sourceSeg.currentIndex === 2
            Layout.leftMargin: 20
            Layout.bottomMargin: 4
            spacing: 8
            Label { text: qsTr("镜像"); font.pixelSize: 12; color: Theme.textDim }
            RComboBox {
                id: mirrorBox
                Layout.preferredWidth: 300
                model: [qsTr("自动（国内优先）")]
                onActivated: {
                    if (!storeWin.searchMode) storeWin.reloadRecommend()
                }
            }
        }

        // 分隔线
        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.separator }

        // 下载任务进度条（贴顶，App Store 不显眼细条）
        Rectangle {
            visible: backend.storeTask.running || backend.storeTask.done
                   || (backend.storeTask.error || "").length > 0
            Layout.fillWidth: true
            Layout.preferredHeight: visible ? 44 : 0
            color: Theme.card

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 20
                anchors.rightMargin: 20
                spacing: 10

                BusyIndicator {
                    visible: backend.storeTask.running
                    running: visible
                    implicitWidth: 16; implicitHeight: 16
                }
                Column {
                    Layout.fillWidth: true
                    spacing: 2
                    Text {
                        text: {
                            const t = backend.storeTask
                            if ((t.error || "").length) return qsTr("✗ ") + t.error
                            if (t.done) return qsTr("✓ ") + (t.label || "") + " " + (t.version || "") + qsTr(" 下载完成，安装中…")
                            return (t.phase || qsTr("准备中…"))
                        }
                        font.pixelSize: 12
                        color: (backend.storeTask.error || "").length ? Theme.danger : Theme.text
                        elide: Text.ElideMiddle
                        width: parent.width
                    }
                    ProgressBar {
                        visible: (backend.storeTask.progress_total || 0) > 0
                        width: parent.width
                        from: 0
                        to: backend.storeTask.progress_total || 1
                        value: backend.storeTask.progress || 0
                    }
                }
                RButton {
                    visible: backend.storeTask.running
                    text: qsTr("取消")
                    Layout.preferredWidth: 60
                    onClicked: backend.storeCancel()
                }
            }
        }
        Rectangle {
            visible: backend.storeTask.running || backend.storeTask.done
                   || (backend.storeTask.error || "").length > 0
            Layout.fillWidth: true; height: 1; color: Theme.separator
        }

        // ============ 内容区 ============
        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: storeWin.searchMode ? 1 : 0

            // ---------- 落地页（推荐，App Store 风） ----------
            Flickable {
                id: homeFlick
                contentWidth: width
                contentHeight: homeCol.implicitHeight + 32
                clip: true
                boundsBehavior: Flickable.StopAtBounds

                ColumnLayout {
                    id: homeCol
                    width: homeFlick.width
                    spacing: 22
                    Layout.topMargin: 0

                    // 加载态 / 错误态
                    Item {
                        visible: backend.storeRecommendLoading
                        Layout.fillWidth: true
                        Layout.preferredHeight: 200
                        Column {
                            anchors.centerIn: parent
                            spacing: 10
                            BusyIndicator { anchors.horizontalCenter: parent.horizontalCenter; running: true }
                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: qsTr("推荐加载中…首次使用需下载 F-Droid 索引（约 15 MB），请稍候")
                                font.pixelSize: 12; color: Theme.textDim
                            }
                        }
                    }
                    Item {
                        visible: !backend.storeRecommendLoading && backend.storeRecommendError.length > 0
                        Layout.fillWidth: true
                        Layout.preferredHeight: 200
                        Column {
                            anchors.centerIn: parent
                            spacing: 10
                            Text { text: "⚠️"; font.pixelSize: 30; anchors.horizontalCenter: parent.horizontalCenter }
                            Text {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: backend.storeRecommendError
                                font.pixelSize: 12; color: Theme.danger
                            }
                            RButton {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: qsTr("重试"); highlighted: true
                                onClicked: storeWin.reloadRecommend()
                            }
                        }
                    }

                    // 主推大卡（Editorial：第一个推荐应用）
                    Item {
                        id: heroCard
                        visible: !backend.storeRecommendLoading && backend.storeRecommendError.length === 0
                                 && backend.storeRecommended.length > 0
                        Layout.fillWidth: true
                        Layout.preferredHeight: 220
                        Layout.leftMargin: 20
                        Layout.rightMargin: 20

                        property var hero: backend.storeRecommended.length > 0 ? backend.storeRecommended[0] : null
                        readonly property string heroPkg: (hero && hero.package) ? hero.package : ""

                        Rectangle {
                            anchors.fill: parent
                            radius: 18
                            gradient: Gradient {
                                GradientStop { position: 0; color: Qt.lighter(Theme.avatarColor(heroCard.heroPkg), 1.25) }
                                GradientStop { position: 1; color: Qt.darker(Theme.avatarColor(heroCard.heroPkg), 1.28) }
                            }
                            clip: true

                            // 右侧超大字母水印
                            Text {
                                anchors.right: parent.right
                                anchors.rightMargin: 24
                                anchors.verticalCenter: parent.verticalCenter
                                text: ((backend.storeRecommended.length > 0 ? (backend.storeRecommended[0].name || "?") : "?")).charAt(0).toUpperCase()
                                font.pixelSize: 170
                                font.weight: Font.Black
                                color: Qt.rgba(1, 1, 1, 0.16)
                            }

                            // 整卡可点：MouseArea 垫底（ColumnLayout/Text 不吃事件，点击
                            // 穿透到它；「查看」按钮在其上层，不再被盖住）
                            MouseArea {
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: backend.storeRecommended.length > 0
                                           && storeWin.openApp(backend.storeRecommended[0])
                            }

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 24
                                spacing: 8

                                Text {
                                    text: qsTr("今日主打")
                                    font.pixelSize: 12; font.weight: Font.DemiBold
                                    color: Qt.rgba(1, 1, 1, 0.85)
                                }
                                RowLayout {
                                    spacing: 14
                                    Layout.fillWidth: true
                                    AppAvatar {
                                        app: backend.storeRecommended.length > 0 ? backend.storeRecommended[0] : null
                                        width: 68; height: 68; radius: 16; pixelSize: 30
                                    }
                                    ColumnLayout {
                                        spacing: 3
                                        Layout.fillWidth: true
                                        Text {
                                            text: backend.storeRecommended.length > 0 ? (backend.storeRecommended[0].name || "") : ""
                                            font.pixelSize: 26; font.weight: Font.Bold
                                            color: "white"
                                            elide: Text.ElideRight
                                            Layout.fillWidth: true
                                        }
                                        Text {
                                            text: backend.storeRecommended.length > 0 ? (backend.storeRecommended[0].summary || "") : ""
                                            font.pixelSize: 13
                                            color: Qt.rgba(1, 1, 1, 0.88)
                                            elide: Text.ElideRight
                                            Layout.fillWidth: true
                                        }
                                    }
                                }
                                Item { Layout.fillHeight: true }
                                RButton {
                                    text: qsTr("查看")
                                    onClicked: backend.storeRecommended.length > 0
                                               && storeWin.openApp(backend.storeRecommended[0])
                                }
                            }
                        }
                    }

                    // 今日推荐（3 卡横排，取推荐 1..3）
                    ColumnLayout {
                        visible: !backend.storeRecommendLoading && backend.storeRecommendError.length === 0
                                 && backend.storeRecommended.length > 1
                        Layout.fillWidth: true
                        Layout.leftMargin: 20
                        Layout.rightMargin: 20
                        spacing: 10

                        Text {
                            text: qsTr("今日推荐")
                            font.pixelSize: 17; font.weight: Font.Bold; color: Theme.text
                        }

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 14

                            Repeater {
                                // 今日推荐：切片 rec[1..3]（与「更多精选」同构，数组驱动
                                // 保证加载完成后 delegate 正确重建）
                                model: {
                                    const rec = backend.storeRecommended
                                    const out = []
                                    for (let i = 1; i < Math.min(4, rec.length); ++i)
                                        out.push(rec[i])
                                    return out
                                }
                                delegate: Rectangle {
                                    id: recCard
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 96
                                    radius: 14
                                    color: recCardMouse.containsMouse ? Theme.cardHover : Theme.card
                                    border.color: Theme.separator

                                    readonly property var app: modelData
                                    readonly property bool installed:
                                        storeWin.installedSet[(app && app.package) || ""] === true

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.margins: 14
                                        spacing: 12

                                        AppAvatar {
                                            app: recCard.app
                                            width: 52; height: 52; radius: 13; pixelSize: 22
                                            Rectangle {
                                                visible: recCard.installed
                                                anchors.top: parent.top; anchors.right: parent.right
                                                anchors.margins: -3
                                                width: 15; height: 15; radius: 7.5
                                                color: Theme.success
                                                border.color: Theme.card; border.width: 2
                                            }
                                        }
                                        ColumnLayout {
                                            spacing: 3
                                            Layout.fillWidth: true
                                            Text {
                                                text: (recCard.app && recCard.app.name) ? recCard.app.name : (recCard.app ? recCard.app.package : "")
                                                font.pixelSize: 14; font.weight: Font.DemiBold
                                                color: Theme.text
                                                elide: Text.ElideRight
                                                Layout.fillWidth: true
                                            }
                                            Text {
                                                text: (recCard.app && recCard.app.summary) ? recCard.app.summary : ""
                                                font.pixelSize: 11; color: Theme.textDim
                                                elide: Text.ElideRight
                                                Layout.fillWidth: true
                                            }
                                            Text {
                                                text: (recCard.app && recCard.app.license) ? recCard.app.license : "F-Droid"
                                                font.pixelSize: 10; color: Theme.textFaint
                                            }
                                        }
                                    }
                                    MouseArea {
                                        id: recCardMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: recCard.app && storeWin.openApp(recCard.app)
                                    }
                                }
                            }
                        }
                    }

                    // 更多精选（紧凑网格，取推荐 4..）
                    ColumnLayout {
                        visible: !backend.storeRecommendLoading && backend.storeRecommendError.length === 0
                                 && backend.storeRecommended.length > 4
                        Layout.fillWidth: true
                        Layout.leftMargin: 20
                        Layout.rightMargin: 20
                        Layout.bottomMargin: 8
                        spacing: 8

                        Text {
                            text: qsTr("更多精选")
                            font.pixelSize: 17; font.weight: Font.Bold; color: Theme.text
                        }

                        GridLayout {
                            Layout.fillWidth: true
                            columns: Math.max(3, Math.floor(storeWin.width / 300))
                            columnSpacing: 6
                            rowSpacing: 2

                            Repeater {
                                model: {
                                    const rec = backend.storeRecommended
                                    const out = []
                                    for (let i = 4; i < rec.length; ++i)
                                        out.push(rec[i])
                                    return out
                                }
                                delegate: Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 56
                                    radius: 10
                                    color: moreMouse.containsMouse ? Theme.cardHover : "transparent"

                                    readonly property var app: modelData
                                    readonly property bool installed: storeWin.installedSet[app.package] === true

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 10
                                        anchors.rightMargin: 8
                                        spacing: 10

                                        AppAvatar {
                                            app: modelData
                                            width: 40; height: 40; radius: 10; pixelSize: 17
                                            Rectangle {
                                                visible: installed
                                                anchors.top: parent.top; anchors.right: parent.right
                                                anchors.margins: -2
                                                width: 12; height: 12; radius: 6
                                                color: Theme.success
                                                border.color: Theme.card; border.width: 1.5
                                            }
                                        }
                                        ColumnLayout {
                                            spacing: 1
                                            Layout.fillWidth: true
                                            Text {
                                                text: app.name || app.package
                                                font.pixelSize: 13; font.weight: Font.Medium
                                                color: Theme.text
                                                elide: Text.ElideRight
                                                Layout.fillWidth: true
                                            }
                                            Text {
                                                text: app.summary || ""
                                                font.pixelSize: 11; color: Theme.textDim
                                                elide: Text.ElideRight
                                                Layout.fillWidth: true
                                            }
                                        }
                                    }
                                    MouseArea {
                                        id: moreMouse
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        cursorShape: Qt.PointingHandCursor
                                        onClicked: storeWin.openApp(app)
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // ---------- 搜索结果页 ----------
            ListView {
                id: resultsList
                clip: true
                spacing: 2
                model: backend.storeResults

                header: Item {
                    width: resultsList.width
                    height: 36
                    Text {
                        anchors.left: parent.left
                        anchors.leftMargin: 20
                        anchors.verticalCenter: parent.verticalCenter
                        text: backend.storeSearching ? qsTr("搜索中…")
                              : backend.storeResults.length === 0 ? qsTr("无匹配结果——换个关键词试试")
                              : qsTr("搜索结果（%1）").arg(backend.storeResults.length)
                        font.pixelSize: 13; color: Theme.textDim
                    }
                }

                delegate: Rectangle {
                    width: resultsList.width
                    height: 68
                    color: resMouse.containsMouse || app.package === storeWin.selectedPkg
                           ? Theme.cardHover : "transparent"

                    readonly property var app: modelData
                    readonly property bool installed: storeWin.installedSet[app.package] === true

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 20
                        anchors.rightMargin: 20
                        spacing: 12

                        AppAvatar {
                            app: modelData
                            width: 44; height: 44; radius: 11; pixelSize: 19
                            Rectangle {
                                visible: installed
                                anchors.top: parent.top; anchors.right: parent.right
                                anchors.margins: -2
                                width: 14; height: 14; radius: 7
                                color: Theme.success
                                border.color: Theme.card; border.width: 2
                            }
                        }
                        ColumnLayout {
                            spacing: 2
                            Layout.fillWidth: true
                            Text {
                                text: app.name || app.package
                                font.pixelSize: 14; font.weight: Font.DemiBold
                                color: Theme.text
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                            Text {
                                text: [app.source === "wandoujia" ? qsTr("豌豆荚")
                                       : app.source === "fdroid" ? "F-Droid" : app.source,
                                       app.developer, app.installCount, app.license]
                                      .filter(function(s) { return s && String(s).length })
                                      .join(" · ")
                                font.pixelSize: 11; color: Theme.textDim
                                elide: Text.ElideMiddle
                                Layout.fillWidth: true
                            }
                        }
                        Text {
                            visible: installed
                            text: qsTr("已装")
                            font.pixelSize: 12; color: Theme.success
                        }
                    }
                    Rectangle {
                        anchors.left: parent.left; anchors.leftMargin: 76
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: 1; color: Theme.separator
                    }
                    MouseArea {
                        id: resMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: storeWin.openApp(app)
                    }
                }
            }
        }
    }

    // ================= 版本面板（底部滑出，App Store 详情页风） =================
    Popup {
        id: versionSheet
        width: storeWin.width
        height: Math.min(storeWin.height * 0.66, 480)
        y: storeWin.height - height
        modal: false
        focus: true
        closePolicy: Popup.CloseOnEscape

        // 点击遮罩外关闭
        onOpened: dimmer.visible = true
        onClosed: dimmer.visible = false

        background: Rectangle {
            color: Theme.card
            radius: 18
            border.color: Theme.separator

            // 顶部拖拽条
            Rectangle {
                anchors.top: parent.top
                anchors.topMargin: 8
                anchors.horizontalCenter: parent.horizontalCenter
                width: 40; height: 5; radius: 2.5
                color: Theme.separator
            }
        }

        // 右上角关闭按钮（z 高于 contentItem，始终可点）
        Rectangle {
            z: 10
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.topMargin: 12
            anchors.rightMargin: 12
            width: 30; height: 30; radius: 15
            color: closeMouse.containsMouse
                   ? (Theme.dark ? Qt.rgba(1,1,1,0.16) : Qt.rgba(0,0,0,0.10))
                   : (Theme.dark ? Qt.rgba(1,1,1,0.08) : Qt.rgba(0,0,0,0.05))
            Text {
                anchors.centerIn: parent
                text: "✕"
                font.pixelSize: 13
                color: Theme.textDim
            }
            MouseArea {
                id: closeMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: versionSheet.close()
            }
            HoverHandler { id: closeHover }
            ToolTip.visible: closeHover.hovered
            ToolTip.text: qsTr("关闭")
            ToolTip.delay: 300
        }

        // 半透遮罩（仅面板上方区域）
        Rectangle {
            id: dimmer
            visible: false
            parent: Overlay.overlay
            anchors.fill: parent
            color: Qt.rgba(0, 0, 0, 0.25)
            MouseArea {
                anchors.fill: parent
                onClicked: versionSheet.close()
            }
            // 面板自身区域点击穿透（把面板区域从遮罩上挖掉）：
            // 用 ExcludedShape 复杂，改为让面板 z 高于遮罩即可——Popup 在 Overlay 上层
        }

        contentItem: Item {
            implicitWidth: versionSheet.width
            implicitHeight: versionSheet.height

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 20
                anchors.topMargin: 24
                spacing: 10

                // 应用头：头像 + 名称 + 简介 + 已装状态
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 14

                    AppAvatar {
                        app: backend.storeApp || null
                        width: 64; height: 64; radius: 15; pixelSize: 28
                    }
                    ColumnLayout {
                        spacing: 3
                        Layout.fillWidth: true
                        Text {
                            text: (backend.storeApp && backend.storeApp.name) ? backend.storeApp.name : ""
                            font.pixelSize: 20; font.weight: Font.Bold; color: Theme.text
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                        Text {
                            visible: !!(backend.storeApp && backend.storeApp.summary
                                        && backend.storeApp.summary.length > 0)
                            text: (backend.storeApp && backend.storeApp.summary) ? backend.storeApp.summary : ""
                            font.pixelSize: 12; color: Theme.textDim
                            elide: Text.ElideRight
                            Layout.fillWidth: true
                        }
                        Text {
                            text: (backend.storeApp && backend.storeApp.license) ? backend.storeApp.license : "F-Droid"
                            font.pixelSize: 11; color: Theme.textFaint
                        }
                    }
                    // 已装状态对照（dumpsys versionCode）
                    Column {
                        spacing: 2
                        visible: backend.storeInstalledCode > 0
                        Text {
                            text: qsTr("已装 %1").arg(
                                (function() {
                                    const c = backend.storeInstalledCode
                                    const vs = backend.storeVersions
                                    for (let i = 0; i < vs.length; ++i)
                                        if ((vs[i].code || 0) === c) return vs[i].name || String(c)
                                    return String(c)
                                })())
                            font.pixelSize: 11; color: Theme.success
                        }
                        Text {
                            visible: backend.storeVersions.length > 0
                                     && (backend.storeVersions[0].code || 0) > backend.storeInstalledCode
                            text: qsTr("有新版本")
                            font.pixelSize: 11; color: Theme.accent
                        }
                    }
                }

                // ========== 下载 / 安装进度区（仅当前应用任务进行中显示） ==========
                // storeTask 的 package 命中当前应用时接管展示：下载 phase → 安装 phase
                Rectangle {
                    id: taskBox
                    Layout.fillWidth: true
                    // 当前应用的下载任务 or 安装任务进行中才显示
                    readonly property bool dlActive:
                        backend.storeTask.running
                        && backend.storeTask.package === (backend.storeApp ? backend.storeApp.package : "")
                    readonly property bool installActive:
                        backend.installTask.running
                    visible: dlActive || installActive
                    Layout.preferredHeight: visible ? 64 : 0
                    radius: 12
                    color: Theme.dark ? Qt.rgba(1,1,1,0.05) : Qt.rgba(0,0,0,0.04)

                    readonly property bool isDl: dlActive
                    readonly property double frac: {
                        const t = isDl ? backend.storeTask : backend.installTask
                        return (t.progress_total || 0) > 0
                               ? Math.min(1, (t.progress || 0) / t.progress_total) : 0
                    }

                    ColumnLayout {
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 6

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 8
                            BusyIndicator {
                                implicitWidth: 15; implicitHeight: 15
                                running: true
                            }
                            Text {
                                text: taskBox.isDl
                                      ? (backend.storeTask.phase || qsTr("下载中…"))
                                      : qsTr("安装中…")
                                font.pixelSize: 12; font.weight: Font.Medium
                                color: Theme.text
                                Layout.fillWidth: true
                                elide: Text.ElideRight
                            }
                            Text {
                                visible: taskBox.frac > 0
                                text: Math.round(taskBox.frac * 100) + "%"
                                font.pixelSize: 12; color: Theme.textDim
                            }
                            Text {
                                visible: taskBox.isDl
                                text: qsTr("取消")
                                font.pixelSize: 12; color: Theme.accent
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: backend.storeCancel()
                                }
                            }
                        }
                        ProgressBar {
                            Layout.fillWidth: true
                            from: 0; to: 1
                            value: taskBox.frac
                            indeterminate: taskBox.frac === 0
                        }
                    }
                }

                Rectangle { Layout.fillWidth: true; height: 1; color: Theme.separator }

                Text {
                    text: backend.storeVersionsLoading ? qsTr("版本加载中…")
                          : qsTr("版本（含历史版本，可回退）")
                    font.pixelSize: 12; color: Theme.textDim
                }

                // 版本列表
                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    spacing: 4
                    model: backend.storeVersions

                        delegate: Rectangle {
                            width: ListView.view ? ListView.view.width - 4 : 300
                            height: 56
                            radius: 10
                            color: verHover.hovered ? Theme.cardHover : "transparent"

                        readonly property var ver: modelData
                        readonly property bool taskRunning:
                            (backend.storeTask.running
                             && backend.storeTask.package === (backend.storeApp ? backend.storeApp.package : ""))
                            || backend.installTask.running
                        readonly property bool isInstalled:
                            (ver.code || 0) > 0
                            && (ver.code || 0) === backend.storeInstalledCode

                        RowLayout {
                            anchors.fill: parent
                            anchors.margins: 10
                            spacing: 10

                            ColumnLayout {
                                spacing: 2
                                Text {
                                    text: (ver.name || "?") + (ver.isLatest ? qsTr("（最新）") : "")
                                    font.pixelSize: 13; color: Theme.text
                                }
                                Text {
                                    text: [ver.size ? storeWin.fmtSize(ver.size) : "",
                                           ver.addedMs
                                             ? new Date(ver.addedMs).toLocaleDateString(Qt.locale(), "yyyy-MM-dd")
                                             : "",
                                           ver.minSdk ? "Android " + ver.minSdk + "+" : ""]
                                          .filter(function(s) { return s && s.length })
                                          .join(" · ")
                                    font.pixelSize: 11; color: Theme.textDim
                                }
                            }
                            Item { Layout.fillWidth: true }
                            Text {
                                visible: isInstalled && !taskRunning
                                text: qsTr("已装")
                                font.pixelSize: 11; color: Theme.success
                            }

                            // 下载/安装按钮状态机：
                            //   获取 → 下载中（进度%）→ 安装中 → 打开
                            //   已装：重新安装 → 同上流程
                            //   失败：重试
                            RButton {
                                id: actionBtn
                                readonly property bool taskRunning:
                                    (backend.storeTask.running
                                     && backend.storeTask.package === (backend.storeApp ? backend.storeApp.package : ""))
                                    || backend.installTask.running
                                readonly property bool taskFailed:
                                    !backend.storeTask.running
                                    && (backend.storeTask.error || "").length > 0
                                readonly property double frac:
                                    (backend.storeTask.progress_total || 0) > 0
                                    ? Math.min(1, (backend.storeTask.progress || 0) / backend.storeTask.progress_total) : 0

                                text: {
                                    if (taskRunning) {
                                        if (backend.installTask.running)
                                            return qsTr("安装中…")
                                        return frac > 0
                                               ? qsTr("下载中 %1%").arg(Math.round(frac * 100))
                                               : qsTr("下载中…")
                                    }
                                    if (taskFailed)
                                        return qsTr("↻ 重试")
                                    if (isInstalled)
                                        return qsTr("⬇ 重新安装")
                                    return qsTr("⬇ 获取")
                                }
                                highlighted: !isInstalled && !taskRunning
                                enabled: !taskRunning
                                onClicked: {
                                    // 任务进行中由头部进度区承载（含取消），按钮只做发起/重试
                                    backend.storeDownload(backend.storeApp, ver)
                                }
                            }
                        }
                        // 行悬停高亮用 HoverHandler：MouseArea 默认接受左键会盖住
                        // RowLayout 里的「获取/重试/重新安装」按钮，点击被吞掉
                        HoverHandler { id: verHover }
                    }
                }
            }
        }
    }
}
