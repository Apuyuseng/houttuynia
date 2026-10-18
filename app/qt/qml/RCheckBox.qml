import QtQuick
import QtQuick.Controls
import Houttuynia

// 圆角复选框：选中主色底白勾，无边框
CheckBox {
    id: control

    indicator: Rectangle {
        implicitWidth: 20
        implicitHeight: 20
        x: control.leftPadding
        y: parent.height / 2 - height / 2
        radius: 6
        color: control.checked
               ? (control.down ? Qt.darker(Theme.accent, 1.12) : Theme.accent)
               : (control.hovered
                  ? (Theme.dark ? "#38383E" : "#E8E8EC")
                  : (Theme.dark ? "#26262B" : "#F0F0F3"))
        Behavior on color { ColorAnimation { duration: 80 } }

        Text {
            anchors.centerIn: parent
            text: "✓"
            font.pixelSize: 13
            font.weight: Font.Bold
            color: "white"
            visible: control.checked
        }
    }

    contentItem: Text {
        text: control.text
        font: control.font
        color: Theme.text
        opacity: control.enabled ? 1 : 0.4
        verticalAlignment: Text.AlignVCenter
        leftPadding: control.indicator.width + control.spacing
    }
}
