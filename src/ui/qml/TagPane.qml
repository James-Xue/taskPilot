import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root
    property var controller

    function focusSearch() {
        searchField.forceActiveFocus()
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 6

        Label {
            text: "Filters"
            font.bold: true
            font.pixelSize: 15
        }
        TextField {
            id: searchField
            Layout.fillWidth: true
            placeholderText: "Search title or notes"
            text: root.controller.searchText
            onTextEdited: root.controller.setSearchText(text)
        }

        Label {
            text: "Tags"
            font.bold: true
            topPadding: 6
        }
        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: root.controller.tagCounts
            ScrollBar.vertical: ScrollBar {}

            delegate: CheckBox {
                id: control
                width: ListView.view.width
                text: modelData.tag + "  (" + modelData.count + ")"
                checked: root.controller.activeTags.indexOf(modelData.tag) >= 0
                onToggled: root.controller.toggleTag(modelData.tag, checked)

                Connections {
                    target: root.controller
                    function onActiveTagsChanged() {
                        control.checked = root.controller.activeTags.indexOf(modelData.tag) >= 0
                    }
                }
            }
        }
        Label {
            visible: root.controller.tagCounts.length === 0
            text: "No tags in the queue"
            opacity: 0.5
            font.pixelSize: 11
        }

        Label {
            text: root.controller.weightsSummary
            opacity: 0.6
            font.pixelSize: 11
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }
        Button {
            text: "Clear filters"
            Layout.fillWidth: true
            onClicked: root.controller.clearFilters()
        }
    }
}
