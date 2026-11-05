import QtQuick
import QtQuick.Controls

// Rounded filled field in the macOS search-field idiom, with the accent ring
// only while focused.
TextField {
    id: control

    property var theme: null

    implicitHeight: 30
    leftPadding: 11
    rightPadding: 11

    font.family: theme ? theme.fontFamily : undefined
    font.pixelSize: 13
    color: theme ? theme.textPrimary : "#1d1d1f"
    placeholderTextColor: theme ? theme.textTertiary : "#6e6e73"
    selectionColor: theme ? theme.accent : "#007aff"
    selectedTextColor: "#ffffff"

    background: Rectangle {
        radius: 8
        color: control.theme ? control.theme.controlBg : "#e9e9ed"
        border.width: control.activeFocus ? 2 : 0
        border.color: control.theme ? control.theme.accent : "#007aff"
    }
}
