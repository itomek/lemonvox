# Presentation layer: theme, lemon icon, indicator bar and tray icon. These
# files talk only to user32/gdi/shell/dwm; OS adapters live in other modules.

add_library(lv_ui STATIC theme.cpp graphics.cpp icon.cpp indicator.cpp tray.cpp)
target_link_libraries(lv_ui PUBLIC lv_common user32 gdi32 shell32 dwmapi uxtheme comctl32 gdiplus shcore ole32 advapi32)
lemonvox_win_target(lv_ui)

# The app icon is rendered from the same vector geometry at build time.
add_executable(lv_icongen icongen.cpp)
target_link_libraries(lv_icongen PRIVATE lv_ui)
lemonvox_win_target(lv_icongen)

set(LEMONVOX_APP_ICON "${CMAKE_CURRENT_BINARY_DIR}/lemonvox.ico" CACHE INTERNAL "Generated LemonVox app icon")
set(LEMONVOX_APP_RC "${CMAKE_CURRENT_BINARY_DIR}/lemonvox.rc" CACHE INTERNAL "LemonVox app resource script")
set(LEMONVOX_APP_MANIFEST "${PROJECT_SOURCE_DIR}/res/lemonvox.manifest" CACHE INTERNAL "LemonVox app manifest")
add_custom_command(OUTPUT "${LEMONVOX_APP_ICON}"
    COMMAND lv_icongen "${LEMONVOX_APP_ICON}"
    DEPENDS lv_icongen
    COMMENT "Rendering lemonvox.ico"
    VERBATIM)
add_custom_target(lv_app_icon DEPENDS "${LEMONVOX_APP_ICON}")
configure_file("${PROJECT_SOURCE_DIR}/res/lemonvox.rc.in" "${LEMONVOX_APP_RC}" @ONLY)

# PerMonitorV2 DPI awareness, common controls v6, asInvoker.
function(lemonvox_win_manifest target)
    target_sources(${target} PRIVATE "${LEMONVOX_APP_MANIFEST}")
endfunction()

# Manifest plus the app icon resource (ID 1). For GUI executables.
function(lemonvox_win_app_resources target)
    lemonvox_win_manifest(${target})
    target_sources(${target} PRIVATE "${LEMONVOX_APP_RC}")
    set_source_files_properties("${LEMONVOX_APP_RC}" PROPERTIES OBJECT_DEPENDS "${LEMONVOX_APP_ICON}")
    add_dependencies(${target} lv_app_icon)
endfunction()

# #23 proof host: tray + indicator driven from a worker through UiThreadExecutor.
add_executable(lemonvox_ui_demo WIN32 ui_demo.cpp)
target_link_libraries(lemonvox_ui_demo PRIVATE lv_ui)
target_include_directories(lemonvox_ui_demo PRIVATE "${PROJECT_SOURCE_DIR}/tests")
lemonvox_win_target(lemonvox_ui_demo)
lemonvox_win_app_resources(lemonvox_ui_demo)
add_test(NAME ui_demo_smoke COMMAND lemonvox_ui_demo --smoke)
set_tests_properties(ui_demo_smoke PROPERTIES LABELS interactive TIMEOUT 1200)

add_executable(ui_unit_test "${PROJECT_SOURCE_DIR}/tests/ui_unit_test.cpp")
target_link_libraries(ui_unit_test PRIVATE lv_ui)
lemonvox_win_test(ui_unit_test)

add_executable(ui_interactive_test "${PROJECT_SOURCE_DIR}/tests/ui_interactive_test.cpp")
target_link_libraries(ui_interactive_test PRIVATE lv_ui)
target_include_directories(ui_interactive_test PRIVATE "${PROJECT_SOURCE_DIR}/tests")
lemonvox_win_manifest(ui_interactive_test)
lemonvox_win_test(ui_interactive_test)
set_tests_properties(ui_interactive_test PROPERTIES LABELS interactive TIMEOUT 1200)

if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/settings.cmake")
    include("${CMAKE_CURRENT_SOURCE_DIR}/settings.cmake")
endif()
