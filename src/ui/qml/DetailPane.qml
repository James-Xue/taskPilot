import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root
    property var controller
    property var task: ({})

    function hasTask() {
        return root.task !== undefined && root.task !== null && root.task.id !== undefined
    }

    function openDeleteDialog() {
        if (root.hasTask()) {
            deleteDialog.open()
        }
    }

    Flickable {
        anchors.fill: parent
        anchors.margins: 16
        clip: true
        contentWidth: width
        contentHeight: content.implicitHeight
        ScrollBar.vertical: ScrollBar {}

        ColumnLayout {
            id: content
            width: parent.width
            spacing: 8

            Label {
                text: root.hasTask() ? root.task.title : "Select a task"
                textFormat: Text.PlainText
                font.pixelSize: 20
                font.bold: true
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }

            GridLayout {
                columns: 2
                visible: root.hasTask()
                rowSpacing: 4
                columnSpacing: 16

                Label { text: "Status"; opacity: 0.6 }
                Label { text: root.hasTask() ? root.task.status : ""; textFormat: Text.PlainText }
                Label { text: "Importance"; opacity: 0.6 }
                Label { text: root.hasTask() ? String(root.task.importance) : "" }
                Label { text: "Due"; opacity: 0.6 }
                Label {
                    text: root.hasTask() ? (root.task.hasDue ? root.task.dueText : "none") : ""
                    textFormat: Text.PlainText
                }
                Label { text: "Blocks"; opacity: 0.6 }
                Label { text: root.hasTask() ? String(root.task.blocks) : "" }
                Label { text: "Tags"; opacity: 0.6 }
                Label {
                    text: root.hasTask() && root.task.tags ? root.task.tags.join(", ") : ""
                    textFormat: Text.PlainText
                    wrapMode: Text.Wrap
                }
            }

            Label {
                visible: root.hasTask()
                text: "Score " + Number(root.task.score).toFixed(2)
                font.pixelSize: 18
                font.bold: true
            }
            Label {
                visible: root.hasTask()
                text: "parts: importance " + Number(root.task.scoreImportance).toFixed(2)
                      + " · urgency " + Number(root.task.scoreUrgency).toFixed(2)
                      + " · age " + Number(root.task.scoreAge).toFixed(2)
                      + " · blocks " + Number(root.task.scoreBlocks).toFixed(2)
                opacity: 0.7
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }

            Label {
                visible: root.hasTask()
                text: "Notes"
                font.bold: true
            }
            ScrollView {
                visible: root.hasTask()
                Layout.fillWidth: true
                Layout.preferredHeight: 100

                TextArea {
                    text: root.hasTask() ? root.task.notes : ""
                    readOnly: true
                    wrapMode: TextArea.Wrap
                }
            }

            RowLayout {
                visible: root.hasTask()

                Button {
                    text: root.task.status === "done" ? "Reopen" : "Complete"
                    enabled: !root.controller.busy
                    onClicked: root.task.status === "done"
                               ? root.controller.reopenTask(root.task.id)
                               : root.controller.completeTask(root.task.id)
                }
                Button {
                    text: "Delete"
                    enabled: !root.controller.busy
                    onClicked: root.openDeleteDialog()
                }
                Item { Layout.fillWidth: true }
            }

            Item { Layout.fillHeight: true }
        }
    }

    Dialog {
        id: deleteDialog
        width: 380
        anchors.centerIn: parent
        modal: true
        title: "Delete task"
        standardButtons: Dialog.Ok | Dialog.Cancel

        onAccepted: root.controller.deleteTask(root.task.id)

        Label {
            width: parent.width
            text: "Delete \"" + (root.hasTask() ? root.task.title : "") + "\"?"
            textFormat: Text.PlainText
            wrapMode: Text.Wrap
        }
    }
}
