# UI tests. Only added when the optional Qt target was configured.

if(NOT TARGET taskpilot-ui-core)
    return()
endif()

find_package(Qt6 QUIET COMPONENTS Test QuickTest Quick)
if(NOT Qt6Test_FOUND OR NOT Qt6QuickTest_FOUND)
    message(STATUS "taskPilot: UI tests disabled (Qt Test/QuickTest not found)")
    return()
endif()

add_executable(tst_queue_model tst_queue_model.cpp)
target_link_libraries(tst_queue_model
    PRIVATE taskpilot-ui-core Qt6::Test taskpilot_warnings)
add_test(NAME tst_queue_model COMMAND tst_queue_model)
set_tests_properties(tst_queue_model PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    TIMEOUT 60)

add_executable(tst_filter_model tst_filter_model.cpp)
target_link_libraries(tst_filter_model
    PRIVATE taskpilot-ui-core Qt6::Test taskpilot_warnings)
add_test(NAME tst_filter_model COMMAND tst_filter_model)
set_tests_properties(tst_filter_model PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    TIMEOUT 60)

add_executable(tst_quick_ui tst_quick_ui.cpp)
target_link_libraries(tst_quick_ui
    PRIVATE taskpilot-ui-core Qt6::Quick Qt6::QuickTest Qt6::Test taskpilot_warnings)
target_compile_definitions(tst_quick_ui PRIVATE
    QUICK_TEST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/qml")
add_test(NAME tst_quick_ui COMMAND tst_quick_ui)
set_tests_properties(tst_quick_ui PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software"
    TIMEOUT 60)

add_executable(tst_render_smoke tst_render_smoke.cpp)
target_link_libraries(tst_render_smoke
    PRIVATE taskpilot-ui-core Qt6::Quick Qt6::Test taskpilot_warnings)
target_compile_definitions(tst_render_smoke PRIVATE
    TASKPILOT_UI_QML_DIR="${PROJECT_SOURCE_DIR}/src/ui/qml")
add_test(NAME tst_render_smoke COMMAND tst_render_smoke)
set_tests_properties(tst_render_smoke PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_QUICK_BACKEND=software"
    TIMEOUT 60)
