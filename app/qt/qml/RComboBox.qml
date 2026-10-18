import QtQuick
import QtQuick.Controls
import Houttuynia

// 圆角无边框下拉框 + 圆角弹层
ComboBox {
    id: control

    background: Rectangle {
        radius: 8
        color: control.popup.visible
               ? (Theme.dark ? "#26262B" : "#ECECEF")
               : Theme.dark ? "#1F1F23" : "#F4F4F6"
        border.width: control.activeFocus ? 1 : 0
        border.color: Theme.accent
        Behavior on color { ColorAnimation { duration: 80 } }
    }

    contentItem: Text {
        leftPadding: 10
        rightPadding: control.indicator.width + 8
        text: control.editable ? control.editText : control.displayText
        font: control.font
        color: Theme.text
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    indicator: Text {
        x: control.width - width - 10
        y: (control.height - height) / 2
        text: "▾"
        font.pixelSize: 12
        color: Theme.textDim
    }

    delegate: ItemDelegate {
        id: item
        width: control.width
        height: 34
        required property var model
        required property int index
        contentItem: Text {
            text: item.modelData !== undefined ? item.modelData
                 : item.model[control.textRole]
            color: item.index === control.currentIndex ? Theme.accent : Theme.text
            font.pixelSize: 13
            verticalAlignment: Text.AlignVCenter
            leftPadding: 8
            elide: Text.ElideRight
        }
        background: Rectangle {
            radius: 7
            color: item.hovered ? (Theme.dark ? "#333338" : "#EDEDF1") : "transparent"
        }
    }

    popup: Popup {
        y: control.height + 6
        width: control.width
        padding: 6
        implicitHeight: Math.min(contentItem.implicitHeight + 12, 320)
        background: Rectangle {
            radius: 10
            color: Theme.card
            border.color: Theme.separator
        }
        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.popup.visible ? control.delegateModel : null
            currentIndex: control.highlightedIndex
            spacing: 2
            ScrollIndicator.vertical: ScrollIndicator {}
        }
        enter: Transition {
            NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 100 }
        }
    }
}
