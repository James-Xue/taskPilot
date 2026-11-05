import QtQuick
import QtQuick.Controls

// Round-cornered 16px checkbox with a white tick, matching macOS form controls.
CheckBox {
    id: control

    property var theme: null

    spacing: 9
    font.family: theme ? theme.fontFamily : undefined
    font.pixelSize: 13
    hoverEnabled: true

    contentItem: Text {
        leftPadding: control.indicator.width + control.spacing
        text: control.text
        font: control.font
        color: control.theme ? control.theme.textPrimary : "#1d1d1f"
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    indicator: Rectangle {
        implicitWidth: 16
        implicitHeight: 16
        x: control.leftPadding
        y: control.topPadding + (control.availableHeight - height) / 2
        radius: 4
        color: control.checked ? (control.theme ? control.theme.accent : "#007aff") : "transparent"
        border.width: control.activeFocus ? 2 : 1.2
        border.color: control.checked
                      ? (control.theme ? control.theme.accent : "#007aff")
                      : (control.activeFocus ? (control.theme ? control.theme.accent : "#007aff")
                                             : (control.theme ? control.theme.textTertiary : "#6e6e73"))

        Text {
            anchors.centerIn: parent
            visible: control.checked
            text: "✓"
            color: "#ffffff"
            font.pixelSize: 11
            font.bold: true
        }
    }
}
