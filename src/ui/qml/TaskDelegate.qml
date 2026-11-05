import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// macOS sidebar row: a rounded selection wash, a two-level text hierarchy and
// the score as the right-aligned secondary figure.
Item {
    id: delegate
    objectName: "taskDelegate"
    property string rowTitle: title
    property bool selected: false
    property var theme: null

    signal clicked()

    readonly property bool hovered: mouse.containsMouse
    implicitHeight: 60

    // Tags arrive as a QStringList from the real model, but a QML ListModel
    // test fixture may hand over a list model instead; normalize both.
    function tagText(value) {
        if (!value) {
            return ""
        }
        if (typeof value === "string") {
            return value
        }
        if (typeof value.join === "function") {
            return value.join(" · ")
        }
        const parts = []
        const count = value.count !== undefined ? value.count : value.length
        for (let i = 0; i < count; ++i) {
            parts.push(value.get ? value.get(i) : value[i])
        }
        return parts.join(" · ")
    }

    function statusText(value) {
        if (!value) {
            return ""
        }
        const text = String(value).replace(/_/g, " ")
        return text.charAt(0).toUpperCase() + text.slice(1)
    }

    function metaText() {
        const parts = []
        // Every row in this pane is actionable; repeating "open" is noise.
        if (status && status !== "open") {
            parts.push(statusText(status))
        }
        parts.push(hasDue ? ("Due " + dueText) : "No due date")
        return parts.join("  ·  ")
    }

    Rectangle {
        anchors.fill: parent
        radius: delegate.theme ? 8 : 8
        color: delegate.selected
               ? (delegate.theme ? delegate.theme.accentSoft : "#e5f0ff")
               : (delegate.hovered ? (delegate.theme ? delegate.theme.hoverBg : "#e2e2e7")
                                   : "transparent")
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        anchors.topMargin: 8
        anchors.bottomMargin: 8
        spacing: 3

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Label {
                text: delegate.rowTitle
                textFormat: Text.PlainText
                font.family: delegate.theme ? delegate.theme.fontFamily : ""
                font.pixelSize: 13
                color: delegate.theme ? delegate.theme.textPrimary : "#1d1d1f"
                elide: Text.ElideRight
                Layout.fillWidth: true
            }
            Label {
                text: Number(score).toFixed(2)
                font.family: delegate.theme ? delegate.theme.fontFamily : ""
                font.pixelSize: 12
                color: delegate.selected
                       ? (delegate.theme ? delegate.theme.accent : "#007aff")
                       : (delegate.theme ? delegate.theme.textSecondary : "#5c5c61")
            }
        }

        Label {
            text: delegate.metaText()
            textFormat: Text.PlainText
            font.family: delegate.theme ? delegate.theme.fontFamily : ""
            font.pixelSize: 11
            color: delegate.theme ? delegate.theme.textSecondary : "#5c5c61"
            elide: Text.ElideRight
            Layout.fillWidth: true
        }

        Label {
            visible: delegate.tagText(tags).length > 0
            text: delegate.tagText(tags)
            textFormat: Text.PlainText
            font.family: delegate.theme ? delegate.theme.fontFamily : ""
            font.pixelSize: 11
            color: delegate.theme ? delegate.theme.textTertiary : "#6e6e73"
            elide: Text.ElideRight
            Layout.fillWidth: true
        }
    }

    MouseArea {
        id: mouse
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor
        onClicked: delegate.clicked()
    }
}
