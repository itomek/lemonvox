# Settings window (included from src/ui/CMakeLists.txt). Depends only on
# lv_common, lemonvox_core and system libraries; the app supplies every other
# module through SettingsServices.
add_library(lv_settings STATIC
    "${CMAKE_CURRENT_LIST_DIR}/settings_window.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/settings_layout.cpp")
target_link_libraries(lv_settings
    PUBLIC lv_common lemonvox_core
    PRIVATE comctl32 dwmapi uxtheme oleacc ole32 oleaut32)
lemonvox_win_target(lv_settings)

add_executable(settings_unit_test "${PROJECT_SOURCE_DIR}/tests/settings_unit_test.cpp")
target_link_libraries(settings_unit_test PRIVATE lv_settings)
target_include_directories(settings_unit_test PRIVATE "${PROJECT_SOURCE_DIR}/tests")
lemonvox_win_test(settings_unit_test)

add_executable(settings_interactive_test "${PROJECT_SOURCE_DIR}/tests/settings_interactive_test.cpp")
target_link_libraries(settings_interactive_test PRIVATE lv_settings ole32 oleaut32 uiautomationcore dwmapi)
target_include_directories(settings_interactive_test PRIVATE "${PROJECT_SOURCE_DIR}/tests")
lemonvox_win_test(settings_interactive_test)
set_tests_properties(settings_interactive_test PROPERTIES LABELS interactive)
