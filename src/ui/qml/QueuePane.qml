import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root
    property var controller
    property var queueModel
    property var theme
    property var currentTask: ({})
    // The selected task identity, kept across a model reset. Row indexes are
    // not identities: a refresh can move the same task to another row, and a
    // surviving index would silently point the detail pane at a different task.
    property int selectedId: -1
    objectName: "queuePane"

    function selectRow(row) {
        if (!root.queueModel || row < 0) {
            return
        }
        const task = root.queueModel.get(row)
        if (!task || task.id === undefined) {
            return
        }
        root.currentTask = task
        root.selectedId = task.id
        list.currentIndex = row
    }

    function findSelectedRow() {
        for (let row = 0; row < list.count; ++row) {
            const candidate = root.queueModel.get(row)
            if (candidate && candidate.id === root.selectedId) {
                return row
            }
        }
        return -1
    }

    function restoreSelection() {
        if (root.selectedId < 0 || !root.queueModel) {
            return
        }
        const row = root.findSelectedRow()
        if (row >= 0) {
            root.currentTask = root.queueModel.get(row)
            list.currentIndex = row
            return
        }
        if (list.count === 0) {
            // An incremental model can clear and repopulate in separate
            // signals; keep the identity until the data says otherwise.
            root.currentTask = ({})
            return
        }
        // A completed reset without the task: it is gone.
        root.currentTask = ({})
        root.selectedId = -1
    }

    function restoreFromInsert() {
        if (root.selectedId < 0) {
            return
        }
        const row = root.findSelectedRow()
        if (row >= 0) {
            root.currentTask = root.queueModel.get(row)
            list.currentIndex = row
        }
    }

    // First run: show the top-ranked task instead of an empty detail pane.
    function ensureSelection() {
        if (root.selectedId < 0 && list.count > 0) {
            root.selectRow(0)
        }
    }

    Connections {
        target: root.queueModel
        function onModelReset() {
            root.restoreSelection()
            root.ensureSelection()
        }
        function onRowsInserted() {
            root.restoreFromInsert()
            root.ensureSelection()
        }
    }

    Component.onCompleted: Qt.callLater(root.ensureSelection)

    Rectangle {
        anchors.fill: parent
        color: root.theme ? root.theme.sidebarBg : "transparent"
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            spacing: 8

            Label {
                text: "Queue"
                font.family: root.theme ? root.theme.fontFamily : ""
                font.pixelSize: 17
                font.weight: Font.DemiBold
                color: root.theme ? root.theme.textPrimary : "#1d1d1f"
            }
            Rectangle {
                radius: 8
                color: root.theme ? root.theme.accentSoft : "#e5f0ff"
                implicitWidth: countLabel.implicitWidth + 14
                implicitHeight: 20

                Label {
                    id: countLabel
                    anchors.centerIn: parent
                    text: list.count
                    font.family: root.theme ? root.theme.fontFamily : ""
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                    color: root.theme ? root.theme.accent : "#007aff"
                }
            }
            Item {
                Layout.fillWidth: true
            }
        }

        Label {
            text: "Ranked by the daemon"
            font.family: root.theme ? root.theme.fontFamily : ""
            font.pixelSize: 12
            color: root.theme ? root.theme.textTertiary : "#6e6e73"
        }

        ListView {
            id: list
            objectName: "queueList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 2
            model: root.queueModel
            // The delegate paints its own rounded selection wash; the Basic
            // style's default highlight would draw a black bar over it.
            highlight: Item {}
            ScrollBar.vertical: AppScrollBar {
                theme: root.theme
            }

            delegate: TaskDelegate {
                width: list.width
                theme: root.theme
                selected: taskId === root.selectedId
                onClicked: root.selectRow(index)
            }
        }

        Label {
            visible: list.count === 0
            text: (root.controller
                   && (root.controller.searchText.length > 0
                       || root.controller.activeTags.length > 0))
                  ? "No tasks match the filters."
                  : "No actionable tasks."
            font.family: root.theme ? root.theme.fontFamily : ""
            font.pixelSize: 12
            color: root.theme ? root.theme.textTertiary : "#6e6e73"
        }
    }
}
