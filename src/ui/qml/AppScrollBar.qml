import QtQuick
import QtQuick.Controls

// Slim translucent overlay scrollbar: visible at rest, stronger while dragging.
ScrollBar {
    id: control

    property var theme: null

    policy: ScrollBar.AsNeeded
    width: 6

    contentItem: Rectangle {
        implicitWidth: 6
        radius: 3
        color: control.theme ? control.theme.textTertiary : "#6e6e73"
        opacity: control.pressed ? 0.75 : 0.35
    }

    background: Item {}
}
