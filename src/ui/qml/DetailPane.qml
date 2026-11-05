import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root
    property var controller
    property var task: ({})
    property var theme

    function hasTask() {
        return root.task !== undefined && root.task !== null && root.task.id !== undefined
    }

    function openDeleteDialog() {
        if (root.hasTask()) {
            deleteDialog.open()
        }
    }

    function statusText(value) {
        if (!value) {
            return ""
        }
        const text = String(value).replace(/_/g, " ")
        return text.charAt(0).toUpperCase() + text.slice(1)
    }

    function summaryText() {
        if (!root.hasTask()) {
            return ""
        }
        const parts = [root.statusText(root.task.status)]
        parts.push(root.task.hasDue ? ("Due " + root.task.dueText) : "No due date")
        if (root.task.tags && root.task.tags.length > 0) {
            parts.push(root.task.tags.join(", "))
        }
        return parts.join("  ·  ")
    }

    Rectangle {
        anchors.fill: parent
        color: root.theme ? root.theme.contentBg : "transparent"
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        Flickable {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: 20
            Layout.rightMargin: 20
            Layout.topMargin: 20
            Layout.bottomMargin: 8
            clip: true
            contentWidth: width
            contentHeight: content.implicitHeight
            ScrollBar.vertical: AppScrollBar {
                theme: root.theme
            }

            ColumnLayout {
                id: content
                width: parent.width
                spacing: 14

                Label {
                    text: root.hasTask() ? root.task.title : "Select a task"
                    textFormat: Text.PlainText
                    font.family: root.theme ? root.theme.fontFamily : ""
                    font.pixelSize: root.hasTask() ? 22 : 17
                    font.weight: Font.DemiBold
                    color: root.hasTask()
                           ? (root.theme ? root.theme.textPrimary : "#1d1d1f")
                           : (root.theme ? root.theme.textSecondary : "#5c5c61")
                    wrapMode: Text.Wrap
                    lineHeightMode: Text.FixedHeight
                    lineHeight: 28
                    Layout.fillWidth: true
                }

                Label {
                    visible: root.hasTask()
                    text: root.summaryText()
                    textFormat: Text.PlainText
                    font.family: root.theme ? root.theme.fontFamily : ""
                    font.pixelSize: 13
                    color: root.theme ? root.theme.textSecondary : "#5c5c61"
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }

                Label {
                    visible: !root.hasTask()
                    text: "Choose a row from the queue to see its details."
                    font.family: root.theme ? root.theme.fontFamily : ""
                    font.pixelSize: 13
                    color: root.theme ? root.theme.textTertiary : "#6e6e73"
                }

                GridLayout {
                    columns: 2
                    visible: root.hasTask()
                    rowSpacing: 9
                    columnSpacing: 28
                    Layout.topMargin: 2

                    Label {
                        text: "Importance"
                        font.family: root.theme ? root.theme.fontFamily : ""
                        font.pixelSize: 12
                        color: root.theme ? root.theme.textSecondary : "#5c5c61"
                    }
                    Label {
                        text: root.hasTask() ? String(root.task.importance) : ""
                        font.family: root.theme ? root.theme.fontFamily : ""
                        font.pixelSize: 13
                        color: root.theme ? root.theme.textPrimary : "#1d1d1f"
                    }
                    Label {
                        text: "Blocks"
                        font.family: root.theme ? root.theme.fontFamily : ""
                        font.pixelSize: 12
                        color: root.theme ? root.theme.textSecondary : "#5c5c61"
                    }
                    Label {
                        text: root.hasTask() ? String(root.task.blocks) : ""
                        font.family: root.theme ? root.theme.fontFamily : ""
                        font.pixelSize: 13
                        color: root.theme ? root.theme.textPrimary : "#1d1d1f"
                    }
                }

                RowLayout {
                    visible: root.hasTask()
                    spacing: 8

                    Label {
                        text: Number(root.task.score).toFixed(2)
                        font.family: root.theme ? root.theme.fontFamily : ""
                        font.pixelSize: 24
                        font.weight: Font.DemiBold
                        color: root.theme ? root.theme.textPrimary : "#1d1d1f"
                    }
                    Label {
                        text: "Score"
                        font.family: root.theme ? root.theme.fontFamily : ""
                        font.pixelSize: 12
                        color: root.theme ? root.theme.textSecondary : "#5c5c61"
                        Layout.alignment: Qt.AlignBottom
                        bottomPadding: 5
                    }
                    Item {
                        Layout.fillWidth: true
                    }
                }

                Label {
                    visible: root.hasTask()
                    text: "importance " + Math.round(root.task.scoreImportance)
                          + "  ·  urgency " + Math.round(root.task.scoreUrgency)
                          + "  ·  age " + Math.round(root.task.scoreAge)
                          + "  ·  blocks " + Math.round(root.task.scoreBlocks)
                    font.family: root.theme ? root.theme.fontFamily : ""
                    font.pixelSize: 12
                    color: root.theme ? root.theme.textSecondary : "#5c5c61"
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }

                Label {
                    visible: root.hasTask()
                    text: "Notes"
                    font.family: root.theme ? root.theme.fontFamily : ""
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                    color: root.theme ? root.theme.textSecondary : "#5c5c61"
                    Layout.topMargin: 4
                }

                Rectangle {
                    visible: root.hasTask()
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(Math.max(notesText.implicitHeight + 32, 96), 260)
                    radius: root.theme ? root.theme.radiusMedium : 10
                    color: root.theme ? root.theme.cardBg : "#f2f2f7"

                    Flickable {
                        id: notesFlick
                        anchors.fill: parent
                        anchors.margins: 16
                        clip: true
                        contentWidth: width
                        contentHeight: notesText.implicitHeight

                        Text {
                            id: notesText
                            width: notesFlick.width
                            text: (root.task.notes && root.task.notes.length > 0)
                                  ? root.task.notes : "No notes."
                            textFormat: Text.PlainText
                            wrapMode: Text.Wrap
                            font.family: root.theme ? root.theme.fontFamily : ""
                            font.pixelSize: 13
                            lineHeight: 1.4
                            color: (root.task.notes && root.task.notes.length > 0)
                                   ? (root.theme ? root.theme.textPrimary : "#1d1d1f")
                                   : (root.theme ? root.theme.textTertiary : "#6e6e73")
                        }
                    }
                }

                Item {
                    Layout.fillHeight: true
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            implicitHeight: root.hasTask() ? 60 : 0
            visible: root.hasTask()
            color: root.theme ? root.theme.contentBg : "#ffffff"

            Rectangle {
                anchors.top: parent.top
                width: parent.width
                height: 1
                color: root.theme ? root.theme.separator : "#dcdce1"
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 20
                anchors.rightMargin: 20
                spacing: 10

                AppButton {
                    text: root.task.status === "done" ? "Reopen" : "Complete"
                    theme: root.theme
                    primary: true
                    enabled: !root.controller.busy
                    onClicked: root.task.status === "done"
                               ? root.controller.reopenTask(root.task.id)
                               : root.controller.completeTask(root.task.id)
                }
                AppButton {
                    text: "Delete"
                    theme: root.theme
                    destructive: true
                    bordered: true
                    enabled: !root.controller.busy
                    onClicked: root.openDeleteDialog()
                }
                Item {
                    Layout.fillWidth: true
                }
            }
        }
    }

    Dialog {
        id: deleteDialog
        width: 400
        anchors.centerIn: parent
        modal: true
        padding: 20

        background: Rectangle {
            radius: root.theme ? root.theme.radiusLarge : 14
            color: root.theme ? root.theme.contentBg : "#ffffff"
            border.width: 1
            border.color: root.theme ? root.theme.separator : "#dcdce1"
        }

        header: Label {
            text: "Delete task"
            font.family: root.theme ? root.theme.fontFamily : ""
            font.pixelSize: 17
            font.weight: Font.DemiBold
            color: root.theme ? root.theme.textPrimary : "#1d1d1f"
            leftPadding: 20
            rightPadding: 20
            topPadding: 18
            bottomPadding: 6
        }

        onOpened: cancelButton.forceActiveFocus()

        Label {
            width: parent.width
            text: "Delete \"" + (root.hasTask() ? root.task.title : "") + "\"? This cannot be undone."
            textFormat: Text.PlainText
            font.family: root.theme ? root.theme.fontFamily : ""
            font.pixelSize: 13
            color: root.theme ? root.theme.textSecondary : "#5c5c61"
            wrapMode: Text.Wrap
        }

        footer: Item {
            implicitHeight: 58

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 20
                anchors.rightMargin: 20
                anchors.bottomMargin: 16
                spacing: 10

                Item {
                    Layout.fillWidth: true
                }
                AppButton {
                    id: cancelButton
                    text: "Cancel"
                    theme: root.theme
                    bordered: true
                    onClicked: deleteDialog.close()
                }
                AppButton {
                    text: "Delete"
                    theme: root.theme
                    destructivePrimary: true
                    onClicked: {
                        root.controller.deleteTask(root.task.id)
                        deleteDialog.close()
                    }
                }
            }
        }
    }
}
