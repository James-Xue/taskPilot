import QtQuick
import QtQuick.Controls

// macOS-style push button: flat by default, a soft grey wash on hover, and a
// filled blue pill for the primary action. `bordered` gives secondary actions
// a quiet persistent bezel so they do not read as labels.
Button {
    id: control

    property var theme: null
    property bool primary: false
    property bool destructive: false
    property bool destructivePrimary: false
    property bool bordered: false

    hoverEnabled: true
    implicitHeight: 28
    padding: 12
    opacity: enabled ? 1 : 0.6

    font.family: theme ? theme.fontFamily : undefined
    font.pixelSize: 13

    contentItem: Text {
        text: control.text
        font: control.font
        color: (control.primary || control.destructivePrimary)
               ? (control.theme ? control.theme.accentText : "#ffffff")
               : (control.destructive && control.theme ? control.theme.danger
                                                       : (control.theme ? control.theme.textPrimary : "#1d1d1f"))
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        radius: control.theme ? control.theme.radiusSmall : 6
        color: {
            if (!control.theme) {
                return control.primary ? "#007aff" : "transparent"
            }
            if (control.primary) {
                return control.down ? Qt.darker(control.theme.accent, 1.12) : control.theme.accent
            }
            if (control.destructivePrimary) {
                return control.down ? Qt.darker(control.theme.danger, 1.12) : control.theme.danger
            }
            if (control.down || control.hovered) {
                return control.theme.hoverBg
            }
            if (control.bordered) {
                return control.theme.controlBg
            }
            return "transparent"
        }
        border.width: control.visualFocus ? 2 : 0
        border.color: (control.primary || control.destructivePrimary)
                      ? Qt.rgba(1, 1, 1, 0.8)
                      : (control.theme ? control.theme.accent : "#007aff")
    }
}
