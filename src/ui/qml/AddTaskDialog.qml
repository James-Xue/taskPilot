import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Dialog {
    id: root
    property var controller
    property var theme: null
    property bool invalid: false
    title: "Add task"
    modal: true
    anchors.centerIn: parent
    width: 480
    padding: 20

    background: Rectangle {
        radius: root.theme ? root.theme.radiusLarge : 14
        color: root.theme ? root.theme.contentBg : "#ffffff"
        border.width: 1
        border.color: root.theme ? root.theme.separator : "#dcdce1"
    }

    header: Label {
        text: root.title
        font.family: root.theme ? root.theme.fontFamily : ""
        font.pixelSize: 17
        font.weight: Font.DemiBold
        color: root.theme ? root.theme.textPrimary : "#1d1d1f"
        leftPadding: 20
        rightPadding: 20
        topPadding: 18
        bottomPadding: 6
    }

    onOpened: {
        invalid = false
        titleField.forceActiveFocus()
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
                text: "Cancel"
                theme: root.theme
                bordered: true
                onClicked: root.reject()
            }
            AppButton {
                text: "Add"
                theme: root.theme
                primary: true
                enabled: !root.controller.busy && titleField.text.trim().length > 0
                onClicked: {
                    // Accept only after the controller validated the input; a
                    // rejected add keeps the dialog open and shows the reason
                    // here instead of in the banner behind the modal.
                    if (root.controller.addTask(titleField.text, importanceSpin.value,
                                                dueField.text, blocksSpin.value,
                                                tagsField.text, notesField.text)) {
                        root.accept()
                    } else {
                        root.invalid = true
                    }
                }
            }
        }
    }

    ColumnLayout {
        width: parent.width
        spacing: 10

        Label {
            text: "Title"
            font.family: root.theme ? root.theme.fontFamily : ""
            font.pixelSize: 12
            color: root.theme ? root.theme.textSecondary : "#5c5c61"
        }
        AppTextField {
            id: titleField
            theme: root.theme
            placeholderText: "What needs doing?"
            Layout.fillWidth: true
            onTextEdited: root.invalid = false
        }

        RowLayout {
            spacing: 10

            Label {
                text: "Importance"
                font.family: root.theme ? root.theme.fontFamily : ""
                font.pixelSize: 12
                color: root.theme ? root.theme.textSecondary : "#5c5c61"
            }
            AppSpinBox {
                id: importanceSpin
                theme: root.theme
                from: 1
                to: 5
                value: 3
            }
            Item {
                Layout.fillWidth: true
            }
            Label {
                text: "Blocks"
                font.family: root.theme ? root.theme.fontFamily : ""
                font.pixelSize: 12
                color: root.theme ? root.theme.textSecondary : "#5c5c61"
            }
            AppSpinBox {
                id: blocksSpin
                theme: root.theme
                from: 0
                to: 999
            }
        }

        Label {
            text: "Due date"
            font.family: root.theme ? root.theme.fontFamily : ""
            font.pixelSize: 12
            color: root.theme ? root.theme.textSecondary : "#5c5c61"
        }
        AppTextField {
            id: dueField
            theme: root.theme
            placeholderText: "yyyy-MM-dd (optional)"
            Layout.fillWidth: true
            onTextEdited: root.invalid = false
        }

        Label {
            text: "Tags"
            font.family: root.theme ? root.theme.fontFamily : ""
            font.pixelSize: 12
            color: root.theme ? root.theme.textSecondary : "#5c5c61"
        }
        AppTextField {
            id: tagsField
            theme: root.theme
            placeholderText: "Comma separated"
            Layout.fillWidth: true
        }

        Label {
            text: "Notes"
            font.family: root.theme ? root.theme.fontFamily : ""
            font.pixelSize: 12
            color: root.theme ? root.theme.textSecondary : "#5c5c61"
        }
        TextArea {
            id: notesField
            placeholderText: "Optional"
            font.family: root.theme ? root.theme.fontFamily : ""
            font.pixelSize: 13
            color: root.theme ? root.theme.textPrimary : "#1d1d1f"
            selectionColor: root.theme ? root.theme.accent : "#007aff"
            selectedTextColor: "#ffffff"
            leftPadding: 11
            rightPadding: 11
            topPadding: 8
            bottomPadding: 8
            wrapMode: TextArea.Wrap
            Layout.fillWidth: true
            Layout.preferredHeight: 72

            background: Rectangle {
                radius: 8
                color: root.theme ? root.theme.controlBg : "#e9e9ed"
                border.width: notesField.activeFocus ? 2 : 0
                border.color: root.theme ? root.theme.accent : "#007aff"
            }
        }

        Label {
            visible: root.invalid
            text: "Please check the title and the due date (yyyy-MM-dd)."
            font.family: root.theme ? root.theme.fontFamily : ""
            font.pixelSize: 12
            color: root.theme ? root.theme.danger : "#ff3b30"
            wrapMode: Text.Wrap
            Layout.fillWidth: true
        }
    }
}
