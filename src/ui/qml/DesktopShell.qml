import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root
    property var controller
    property var queueModel

    function openAddDialog() {
        addTaskDialog.open()
    }

    function focusSearch() {
        tagPane.focusSearch()
    }

    function toggleSelected() {
        const task = queuePane.currentTask
        if (!task || task.id === undefined) {
            return
        }
        if (task.status === "done") {
            root.controller.reopenTask(task.id)
        } else {
            root.controller.completeTask(task.id)
        }
    }

    function requestDelete() {
        detailPane.openDeleteDialog()
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        ToolBar {
            Layout.fillWidth: true

            RowLayout {
                anchors.fill: parent
                spacing: 8

                Button {
                    text: "Add"
                    enabled: !root.controller.busy
                    onClicked: root.openAddDialog()
                }
                Button {
                    text: "Refresh"
                    enabled: !root.controller.busy
                    onClicked: root.controller.refresh()
                }
                Item {
                    Layout.fillWidth: true
                }
                Label {
                    text: root.controller.sourceLabel
                    textFormat: Text.PlainText
                    color: root.controller.offline ? "#ff6b6b"
                           : (root.controller.busy ? "#f0c674" : "#7bd88f")
                }
                BusyIndicator {
                    running: root.controller.busy
                    visible: running
                    Layout.preferredWidth: 22
                    Layout.preferredHeight: 22
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            visible: root.controller.errorText.length > 0
            color: "#5a1f1f"
            implicitHeight: visible ? errorLabel.implicitHeight + 16 : 0

            Label {
                id: errorLabel
                anchors.fill: parent
                anchors.margins: 8
                text: root.controller.errorText
                textFormat: Text.PlainText
                color: "#ffd7d7"
                wrapMode: Text.Wrap
            }
        }

        SplitView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: Qt.Horizontal

            QueuePane {
                id: queuePane
                SplitView.preferredWidth: 380
                SplitView.minimumWidth: 280
                controller: root.controller
                queueModel: root.queueModel
            }
            DetailPane {
                id: detailPane
                SplitView.fillWidth: true
                SplitView.minimumWidth: 360
                controller: root.controller
                task: queuePane.currentTask
            }
            TagPane {
                id: tagPane
                SplitView.preferredWidth: 280
                SplitView.minimumWidth: 220
                controller: root.controller
            }
        }

        Rectangle {
            Layout.fillWidth: true
            height: 30
            color: "#1b1d1f"

            Label {
                anchors.fill: parent
                anchors.leftMargin: 8
                verticalAlignment: Text.AlignVCenter
                text: root.controller.statusSummary
                textFormat: Text.PlainText
                elide: Text.ElideRight
            }
        }
    }

    AddTaskDialog {
        id: addTaskDialog
        controller: root.controller
    }
}
