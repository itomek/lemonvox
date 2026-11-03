add_library(lv_prefs STATIC preferences.cpp)
target_link_libraries(lv_prefs PUBLIC lv_common PRIVATE advapi32)
lemonvox_win_target(lv_prefs)

add_executable(prefs_test "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/prefs_test.cpp")
target_link_libraries(prefs_test PRIVATE lv_prefs advapi32)
lemonvox_win_test(prefs_test)
