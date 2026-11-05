import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root
    property var controller
    property var queueModel
    property var theme
    property bool darkTheme: true

    signal toggleTheme()

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

        // Unified titlebar: app name on the left, actions on the right. No
        // coloured chrome; the surface is the window itself.
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: root.theme.toolbarHeight
            color: root.theme.windowBg

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 20
                anchors.rightMargin: 20
                spacing: 10

                Label {
                    text: "taskPilot"
                    font.family: root.theme.fontFamily
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                    color: root.theme.textPrimary
                }
                Item {
                    Layout.fillWidth: true
                }

                AppButton {
                    text: "Add"
                    theme: root.theme
                    primary: true
                    enabled: !root.controller.busy
                    onClicked: root.openAddDialog()
                }
                AppButton {
                    text: "↻ Refresh"
                    theme: root.theme
                    enabled: !root.controller.busy
                    onClicked: root.controller.refresh()
                }
                AppButton {
                    text: root.darkTheme ? "☀ Light" : "☾ Dark"
                    theme: root.theme
                    onClicked: root.toggleTheme()

                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                    ToolTip.text: root.darkTheme ? "Switch to light appearance"
                                                 : "Switch to dark appearance"
                }

                Rectangle {
                    width: 1
                    height: 16
                    color: root.theme.separator
                    Layout.leftMargin: 4
                    Layout.rightMargin: 4
                }

                RowLayout {
                    spacing: 6

                    Rectangle {
                        width: 7
                        height: 7
                        radius: 3.5
                        color: root.controller.offline ? root.theme.danger
                               : (root.controller.busy ? root.theme.warning : root.theme.success)
                    }
                    Label {
                        text: root.controller.sourceLabel
                        font.family: root.theme.fontFamily
                        font.pixelSize: 11
                        color: root.theme.textSecondary
                        textFormat: Text.PlainText
                    }
                }
                BusyIndicator {
                    running: root.controller.busy
                    visible: running
                    implicitWidth: 18
                    implicitHeight: 18
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            height: 1
            color: root.theme.separator
        }

        Rectangle {
            Layout.fillWidth: true
            visible: root.controller.errorText.length > 0
            color: root.theme.dark
                   ? Qt.rgba(255 / 255, 69 / 255, 58 / 255, 0.14)
                   : Qt.rgba(255 / 255, 59 / 255, 48 / 255, 0.10)
            implicitHeight: visible ? errorLabel.implicitHeight + 16 : 0

            Label {
                id: errorLabel
                anchors.fill: parent
                anchors.margins: 8
                text: root.controller.errorText
                textFormat: Text.PlainText
                font.family: root.theme.fontFamily
                font.pixelSize: 12
                color: root.theme.danger
                wrapMode: Text.Wrap
            }
        }

        SplitView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: Qt.Horizontal
            handle: Item {
                implicitWidth: 9
                implicitHeight: 9

                Rectangle {
                    anchors.horizontalCenter: parent.horizontalCenter
                    width: 1
                    height: parent.height
                    color: root.theme.separator
                }
                HoverHandler {
                    cursorShape: Qt.SplitHCursor
                }
            }

            QueuePane {
                id: queuePane
                SplitView.preferredWidth: 380
                SplitView.minimumWidth: 280
                controller: root.controller
                queueModel: root.queueModel
                theme: root.theme
            }
            DetailPane {
                id: detailPane
                SplitView.fillWidth: true
                SplitView.minimumWidth: 360
                controller: root.controller
                task: queuePane.currentTask
                theme: root.theme
            }
            TagPane {
                id: tagPane
                SplitView.preferredWidth: 280
                SplitView.minimumWidth: 220
                controller: root.controller
                theme: root.theme
            }
        }

        Rectangle {
            Layout.fillWidth: true
            height: 26
            color: root.theme.windowBg

            Rectangle {
                anchors.top: parent.top
                width: parent.width
                height: 1
                color: root.theme.separator
            }

            Label {
                anchors.fill: parent
                anchors.leftMargin: 20
                verticalAlignment: Text.AlignVCenter
                text: root.controller.statusSummary
                textFormat: Text.PlainText
                font.family: root.theme.fontFamily
                font.pixelSize: 11
                color: root.theme.textSecondary
                elide: Text.ElideRight
            }
        }
    }

    AddTaskDialog {
        id: addTaskDialog
        controller: root.controller
        theme: root.theme
    }
}
