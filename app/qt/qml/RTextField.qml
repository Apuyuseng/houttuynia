import QtQuick
import QtQuick.Controls
import Houttuynia

// 圆角无边框输入框：底色与卡片区分，聚焦时主色描边
TextField {
    id: control

    background: Rectangle {
        radius: 8
        color: Theme.dark ? "#1F1F23" : "#F4F4F6"
        border.width: control.activeFocus ? 1 : 0
        border.color: Theme.accent
        Behavior on border.width { NumberAnimation { duration: 80 } }
    }

    color: Theme.text
    placeholderTextColor: Theme.textFaint
    selectionColor: Theme.accent
    selectedTextColor: "white"
    selectByMouse: true
    padding: 7
    leftPadding: 10
    rightPadding: 10
}
