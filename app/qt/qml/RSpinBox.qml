import QtQuick
import QtQuick.Controls
import Houttuynia

// 圆角无边框步进器（保留上下调节钮）
SpinBox {
    id: control

    background: Rectangle {
        radius: 8
        color: Theme.dark ? "#1F1F23" : "#F4F4F6"
        border.width: control.activeFocus ? 1 : 0
        border.color: Theme.accent
    }

    contentItem: TextInput {
        z: 2
        text: control.textFromValue(control.value, control.locale)
        font: control.font
        color: Theme.text
        selectionColor: Theme.accent
        selectedTextColor: "white"
        horizontalAlignment: Qt.AlignHCenter
        verticalAlignment: Qt.AlignVCenter
        readOnly: !control.editable
        validator: control.validator
        inputMethodHints: Qt.ImhFormattedNumbersOnly
    }

    up.indicator: Rectangle {
        x: control.width - width
        height: control.height
        width: 28
        radius: 8
        color: control.up.pressed
               ? (Theme.dark ? "#3C3C42" : "#DCDCE2")
               : control.up.hovered
                 ? (Theme.dark ? "#38383E" : "#E4E4E9")
                 : "transparent"
        Text {
            anchors.centerIn: parent
            text: "＋"
            font.pixelSize: 13
            color: control.up.hovered ? Theme.text : Theme.textDim
        }
    }

    down.indicator: Rectangle {
        x: 0
        height: control.height
        width: 28
        radius: 8
        color: control.down.pressed
               ? (Theme.dark ? "#3C3C42" : "#DCDCE2")
               : control.down.hovered
                 ? (Theme.dark ? "#38383E" : "#E4E4E9")
                 : "transparent"
        Text {
            anchors.centerIn: parent
            text: "－"
            font.pixelSize: 13
            color: control.down.hovered ? Theme.text : Theme.textDim
        }
    }
}
