import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: window
    visible: true
    width: 1280
    height: 800
    minimumWidth: 900
    minimumHeight: 600
    title: "taskPilot"

    Material.theme: Material.Dark
    Material.accent: "#4c8dff"
    Material.primary: "#2b6cb0"

    DesktopShell {
        id: shell
        anchors.fill: parent
        controller: appController
        queueModel: taskQueueModel
    }

    Shortcut {
        sequences: ["Ctrl+N"]
        onActivated: shell.openAddDialog()
    }
    Shortcut {
        sequences: ["F5", "Ctrl+R"]
        onActivated: appController.refresh()
    }
    Shortcut {
        sequences: ["Ctrl+F"]
        onActivated: shell.focusSearch()
    }
    Shortcut {
        sequence: "Space"
        onActivated: shell.toggleSelected()
    }
    Shortcut {
        sequence: "Delete"
        onActivated: shell.requestDelete()
    }
    Shortcut {
        sequence: "Escape"
        onActivated: appController.clearFilters()
    }

    // Refresh when the window regains focus, so a daemon that died while the
    // app was idle is noticed without polling the socket forever.
    onActiveChanged: {
        if (active) {
            appController.refresh()
        }
    }

    Component.onCompleted: appController.refresh()
}
