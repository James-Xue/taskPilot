import QtQuick
import QtQuick.Controls
import QtCore

ApplicationWindow {
    id: window
    visible: true
    width: 1280
    height: 800
    minimumWidth: 900
    minimumHeight: 600
    title: "taskPilot"
    color: theme.windowBg

    Settings {
        id: uiSettings
        category: "ui"
        property string theme: "dark"
    }

    readonly property bool darkTheme: uiSettings.theme !== "light"

    Theme {
        id: theme
        dark: window.darkTheme
    }

    DesktopShell {
        id: shell
        objectName: "desktopShell"
        anchors.fill: parent
        controller: appController
        queueModel: taskQueueModel
        theme: theme
        darkTheme: window.darkTheme
        onToggleTheme: uiSettings.theme = window.darkTheme ? "light" : "dark"
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
