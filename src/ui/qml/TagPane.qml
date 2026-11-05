import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root
    property var controller
    property var theme

    function focusSearch() {
        searchField.forceActiveFocus()
    }

    Rectangle {
        anchors.fill: parent
        color: root.theme ? root.theme.sidebarBg : "transparent"
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 10

        Label {
            text: "Filters"
            font.family: root.theme ? root.theme.fontFamily : ""
            font.pixelSize: 17
            font.weight: Font.DemiBold
            color: root.theme ? root.theme.textPrimary : "#1d1d1f"
        }

        AppTextField {
            id: searchField
            Layout.fillWidth: true
            theme: root.theme
            placeholderText: "Search tasks"
            text: root.controller.searchText
            onTextEdited: root.controller.setSearchText(text)
        }

        Label {
            text: "Tags"
            topPadding: 4
            font.family: root.theme ? root.theme.fontFamily : ""
            font.pixelSize: 12
            font.weight: Font.DemiBold
            color: root.theme ? root.theme.textSecondary : "#5c5c61"
        }

        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 2
            model: root.controller.tagCounts
            ScrollBar.vertical: AppScrollBar {
                theme: root.theme
            }

            delegate: RowLayout {
                width: ListView.view.width
                spacing: 4

                AppCheckBox {
                    id: control
                    Layout.fillWidth: true
                    theme: root.theme
                    text: modelData.tag
                    checked: root.controller.activeTags.indexOf(modelData.tag) >= 0
                    onToggled: root.controller.toggleTag(modelData.tag, checked)

                    Connections {
                        target: root.controller
                        function onActiveTagsChanged() {
                            control.checked = root.controller.activeTags.indexOf(modelData.tag) >= 0
                        }
                    }
                }
                Label {
                    text: modelData.count
                    font.family: root.theme ? root.theme.fontFamily : ""
                    font.pixelSize: 12
                    color: root.theme ? root.theme.textTertiary : "#6e6e73"
                    Layout.rightMargin: 4
                }
            }
        }

        Label {
            visible: root.controller.tagCounts.length === 0
            text: "No tags in the queue"
            font.family: root.theme ? root.theme.fontFamily : ""
            font.pixelSize: 11
            color: root.theme ? root.theme.textTertiary : "#6e6e73"
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: weightsLabel.implicitHeight + 16
            radius: root.theme ? root.theme.radiusSmall : 6
            color: root.theme ? root.theme.cardBg : "#f2f2f7"

            Label {
                id: weightsLabel
                anchors.fill: parent
                anchors.margins: 8
                text: root.controller.weightsSummary
                font.family: root.theme ? root.theme.fontFamily : ""
                font.pixelSize: 11
                color: root.theme ? root.theme.textSecondary : "#5c5c61"
                wrapMode: Text.Wrap
            }
        }

        AppButton {
            text: "Clear filters"
            theme: root.theme
            bordered: true
            Layout.fillWidth: true
            enabled: root.controller.searchText.length > 0
                     || root.controller.activeTags.length > 0
            onClicked: root.controller.clearFilters()
        }
    }
}
