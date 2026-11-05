import QtQuick
import QtTest
import "../../../src/ui/qml"

TestCase {
    id: testCase
    name: "QueuePage"
    when: windowShown

    Component {
        id: modelComponent
        ListModel {}
    }

    Component {
        id: controllerComponent
        QtObject {
            property bool busy: false
            property bool offline: false
            property string searchText: ""
            property var activeTags: []
        }
    }

    Component {
        id: paneComponent
        QueuePane {
            width: 380
            height: 560
        }
    }

    function addRow(model, id, title, score) {
        model.append({
            id: id,
            taskId: id,
            uid: "uid-" + id,
            title: title,
            notes: "",
            status: "open",
            importance: 3,
            blocks: 0,
            tags: [],
            dueText: "",
            hasDue: false,
            score: score,
            scoreImportance: 1,
            scoreUrgency: 2,
            scoreAge: 3,
            scoreBlocks: 4,
            urgencyFactor: 0.1,
            ageDays: 2
        })
    }

    function test_rendered_order_follows_model_order() {
        var model = createTemporaryObject(modelComponent, testCase)
        verify(model !== null)
        addRow(model, 1, "Alpha", 30.0)
        addRow(model, 2, "Beta", 20.0)
        addRow(model, 3, "Charlie", 10.0)

        var controller = createTemporaryObject(controllerComponent, testCase)
        var pane = createTemporaryObject(paneComponent, testCase,
                                         { controller: controller, queueModel: model })
        verify(pane !== null)

        var list = findChild(pane, "queueList")
        verify(list !== null)
        compare(list.count, 3)
        wait(50)

        compare(list.itemAtIndex(0).rowTitle, "Alpha")
        compare(list.itemAtIndex(1).rowTitle, "Beta")
        compare(list.itemAtIndex(2).rowTitle, "Charlie")
    }

    function test_selection_follows_the_task_across_a_model_reset() {
        var model = createTemporaryObject(modelComponent, testCase)
        verify(model !== null)
        addRow(model, 1, "Alpha", 30.0)
        addRow(model, 2, "Beta", 20.0)
        addRow(model, 3, "Charlie", 10.0)

        var controller = createTemporaryObject(controllerComponent, testCase)
        var pane = createTemporaryObject(paneComponent, testCase,
                                         { controller: controller, queueModel: model })
        verify(pane !== null)

        var list = findChild(pane, "queueList")
        verify(list !== null)
        pane.selectRow(1)
        wait(20)
        compare(pane.selectedId, 2)
        compare(pane.currentTask.title, "Beta")

        // A refresh reorders the same tasks. The row index alone would point
        // at a different task, so selection must follow the task id.
        model.clear()
        addRow(model, 3, "Charlie", 10.0)
        addRow(model, 2, "Beta", 20.0)
        addRow(model, 1, "Alpha", 30.0)
        wait(50)

        compare(pane.selectedId, 2)
        compare(pane.currentTask.title, "Beta")
        verify(list.itemAtIndex(1).selected)
    }
}
