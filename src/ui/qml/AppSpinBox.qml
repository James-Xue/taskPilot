import QtQuick
import QtQuick.Controls

// Compact numeric stepper with the same filled surface as AppTextField.
SpinBox {
    id: control

    property var theme: null

    implicitHeight: 28
    implicitWidth: 96

    font.family: theme ? theme.fontFamily : undefined
    font.pixelSize: 13

    contentItem: TextInput {
        z: 2
        text: control.textFromValue(control.value, control.locale)
        font: control.font
        color: control.theme ? control.theme.textPrimary : "#1d1d1f"
        selectionColor: control.theme ? control.theme.accent : "#007aff"
        selectedTextColor: "#ffffff"
        horizontalAlignment: Qt.AlignHCenter
        verticalAlignment: Qt.AlignVCenter
        readOnly: !control.editable
        validator: control.validator
        inputMethodHints: control.inputMethodHints
        leftPadding: control.down.indicator.width + 4
        rightPadding: control.up.indicator.width + 4
    }

    up.indicator: Rectangle {
        x: control.width - width - control.rightPadding
        y: control.topPadding
        implicitWidth: 22
        implicitHeight: control.height - control.topPadding - control.bottomPadding
        color: control.up.pressed ? (control.theme ? control.theme.hoverBg : "#e2e2e7") : "transparent"

        Text {
            anchors.centerIn: parent
            text: "+"
            font.pixelSize: 13
            color: control.up.enabled ? (control.theme ? control.theme.textSecondary : "#5c5c61")
                                      : (control.theme ? control.theme.textTertiary : "#6e6e73")
        }
    }

    down.indicator: Rectangle {
        x: control.leftPadding
        y: control.topPadding
        implicitWidth: 22
        implicitHeight: control.height - control.topPadding - control.bottomPadding
        color: control.down.pressed ? (control.theme ? control.theme.hoverBg : "#e2e2e7") : "transparent"

        Text {
            anchors.centerIn: parent
            text: "−"
            font.pixelSize: 13
            color: control.down.enabled ? (control.theme ? control.theme.textSecondary : "#5c5c61")
                                        : (control.theme ? control.theme.textTertiary : "#6e6e73")
        }
    }

    background: Rectangle {
        radius: 8
        color: control.theme ? control.theme.controlBg : "#e9e9ed"
        border.width: control.activeFocus ? 2 : 0
        border.color: control.theme ? control.theme.accent : "#007aff"
    }
}
