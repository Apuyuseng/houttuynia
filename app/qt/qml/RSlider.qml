import QtQuick
import QtQuick.Controls
import Houttuynia

// 圆形手柄滑条
Slider {
    id: control

    background: Rectangle {
        x: control.leftPadding
        y: control.topPadding + control.availableHeight / 2 - height / 2
        width: control.availableWidth
        height: 5
        radius: 2.5
        color: Theme.dark ? "#333338" : "#E4E4E9"

        Rectangle {
            width: control.visualPosition * parent.width
            height: parent.height
            radius: 2.5
            color: Theme.accent
        }
    }

    handle: Rectangle {
        x: control.leftPadding + control.visualPosition
           * (control.availableWidth - width)
        y: control.topPadding + control.availableHeight / 2 - height / 2
        width: 18
        height: 18
        radius: 9
        color: control.pressed ? Qt.darker(Theme.accent, 1.15)
               : control.hovered ? Qt.darker(Theme.accent, 1.08)
               : Theme.accent
        border.width: 3
        border.color: Theme.card
        Behavior on color { ColorAnimation { duration: 80 } }
    }

    implicitHeight: 24
}
