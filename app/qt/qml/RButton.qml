import QtQuick
import QtQuick.Controls
import Houttuynia

// 圆角无边框按钮：常态浅灰底、高亮态主色底，深浅色自适应
Button {
    id: control

    background: Rectangle {
        radius: 8
        color: !control.enabled
               ? (Theme.dark ? "#2E2E33" : "#ECECEF")
               : control.highlighted
                 ? (control.pressed ? Qt.darker(Theme.accent, 1.15)
                    : control.hovered ? Qt.darker(Theme.accent, 1.08) : Theme.accent)
                 : control.pressed
                   ? (Theme.dark ? "#3C3C42" : "#DCDCE2")
                   : control.hovered
                     ? (Theme.dark ? "#38383E" : "#E4E4E9")
                     : (Theme.dark ? "#333338" : "#E8E8EC")
        Behavior on color { ColorAnimation { duration: 80 } }
    }

    contentItem: Text {
        text: control.text
        font: control.font
        color: !control.enabled ? Theme.textFaint
               : control.highlighted ? "white" : Theme.text
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    padding: 6
    leftPadding: 14
    rightPadding: 14
}
