import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: root
    property var controller
    title: "Add task"
    modal: true
    anchors.centerIn: parent
    width: 440

    footer: DialogButtonBox {
        Button {
            text: "Cancel"
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            onClicked: root.reject()
        }
        Button {
            text: "Add"
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            enabled: !root.controller.busy
            onClicked: {
                // Accept only after the controller validated the input; an
                // invalid title or date keeps the dialog open with the error
                // in the banner behind it.
                if (root.controller.addTask(titleField.text, importanceSpin.value,
                                            dueField.text, blocksSpin.value,
                                            tagsField.text, notesField.text)) {
                    root.accept()
                }
            }
        }
    }

    ColumnLayout {
        width: parent.width
        spacing: 8

        TextField {
            id: titleField
            placeholderText: "Title"
            Layout.fillWidth: true
            focus: true
        }
        RowLayout {
            Label { text: "Importance" }
            SpinBox {
                id: importanceSpin
                from: 1
                to: 5
                value: 3
            }
            Item { Layout.fillWidth: true }
            Label { text: "Blocks" }
            SpinBox {
                id: blocksSpin
                from: 0
                to: 999
            }
        }
        TextField {
            id: dueField
            placeholderText: "Due date yyyy-MM-dd (optional)"
            Layout.fillWidth: true
        }
        TextField {
            id: tagsField
            placeholderText: "Tags, comma separated"
            Layout.fillWidth: true
        }
        TextArea {
            id: notesField
            placeholderText: "Notes"
            Layout.fillWidth: true
            Layout.preferredHeight: 80
            wrapMode: TextArea.Wrap
        }
    }
}
