import QtQuick
import QtQuick.Controls
import Houttuynia

// 圆角卡片分组：无边框、卡片底、圆角 12，标题随主题
GroupBox {
    id: control

    background: Rectangle {
        radius: 12
        color: Theme.card
    }

    label: Text {
        x: control.leftPadding
        y: 4
        width: control.availableWidth
        text: control.title
        font.pixelSize: 13
        font.weight: Font.Bold
        color: Theme.text
        elide: Text.ElideRight
        visible: control.title.length > 0
    }

    topPadding: control.title.length > 0 ? 30 : 12
    bottomPadding: 12
    leftPadding: 14
    rightPadding: 14
}
