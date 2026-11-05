import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Item {
    id: root
    property var controller
    property var queueModel
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

    Connections {
        target: root.queueModel
        function onModelReset() {
            root.restoreSelection()
        }
        function onRowsInserted() {
            root.restoreFromInsert()
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 6

        Label {
            text: "Queue · " + list.count
            font.bold: true
            font.pixelSize: 15
        }
        Label {
            text: "Ranked by the daemon"
            opacity: 0.6
            font.pixelSize: 11
        }

        ListView {
            id: list
            objectName: "queueList"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 2
            model: root.queueModel
            ScrollBar.vertical: ScrollBar {}

            delegate: TaskDelegate {
                width: list.width
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
            opacity: 0.6
        }
    }
}
