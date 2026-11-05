import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ItemDelegate {
    id: delegate
    objectName: "taskDelegate"
    property string rowTitle: title
    property bool selected: false
    width: ListView.view ? ListView.view.width : implicitWidth
    highlighted: selected

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

    contentItem: ColumnLayout {
        spacing: 2

        Label {
            text: delegate.rowTitle
            textFormat: Text.PlainText
            font.bold: true
            elide: Text.ElideRight
            Layout.fillWidth: true
        }
        RowLayout {
            spacing: 8

            Label {
                text: status
                textFormat: Text.PlainText
                font.pixelSize: 11
                opacity: 0.7
            }
            Label {
                text: hasDue ? ("due " + dueText) : "no due date"
                textFormat: Text.PlainText
                font.pixelSize: 11
                opacity: 0.7
            }
            Item {
                Layout.fillWidth: true
            }
            Label {
                text: Number(score).toFixed(2)
                font.bold: true
            }
        }
        Label {
            visible: delegate.tagText(tags).length > 0
            text: delegate.tagText(tags)
            textFormat: Text.PlainText
            font.pixelSize: 11
            opacity: 0.5
            elide: Text.ElideRight
            Layout.fillWidth: true
        }
    }
}
