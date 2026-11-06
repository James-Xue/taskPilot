# The desktop Qt Quick client. Qt is optional at the root; this file is only
# added when Qt 6.12 was found.

add_library(taskpilot-ui-core STATIC
    queue_model.cpp
    task_filter_model.cpp
)
target_link_libraries(taskpilot-ui-core
    PUBLIC
        Qt6::Core
        taskPilot::result
    PRIVATE
        taskpilot_warnings)
target_compile_features(taskpilot-ui-core PUBLIC cxx_std_20)

add_library(taskpilot-ui-desktop STATIC
    control_worker.cpp
)
target_link_libraries(taskpilot-ui-desktop
    PUBLIC
        taskpilot-ui-core
        taskPilot::client
    PRIVATE
        taskpilot_warnings)

# AppController owns the worker thread, so it lives above the desktop client
# library rather than in ui-core. Keeping it out of ui-core matters: a test
# that links only the models must not drag the socket client in through the
# moc compilation unit.
add_library(taskpilot-ui-controller STATIC
    app_controller.cpp
)
target_link_libraries(taskpilot-ui-controller
    PUBLIC
        taskpilot-ui-core
        taskpilot-ui-desktop
        nlohmann_json::nlohmann_json
    PRIVATE
        taskpilot_warnings)

qt_add_executable(taskpilot-ui main.cpp)
target_link_libraries(taskpilot-ui
    PRIVATE
        taskpilot-ui-controller
        taskpilot-ui-desktop
        Qt6::Quick
        Qt6::QuickControls2
        taskpilot_warnings)
set_target_properties(taskpilot-ui PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

qt_add_resources(taskpilot-ui "ui-qml"
    PREFIX "/ui"
    FILES
        qml/Main.qml
        qml/DesktopShell.qml
        qml/QueuePane.qml
        qml/TaskDelegate.qml
        qml/DetailPane.qml
        qml/TagPane.qml
        qml/AddTaskDialog.qml
        qml/Theme.qml
        qml/AppButton.qml
        qml/AppTextField.qml
        qml/AppCheckBox.qml
        qml/AppSpinBox.qml
        qml/AppScrollBar.qml
        icons/taskpilot.svg
        icons/taskpilot-64.png
        icons/taskpilot-256.png
        icons/taskpilot-512.png)
