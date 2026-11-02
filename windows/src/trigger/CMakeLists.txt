add_library(lv_trigger STATIC
    key_normalizer.cpp
    global_trigger.cpp
    shortcut_recorder.cpp)
target_link_libraries(lv_trigger PUBLIC lv_common user32)
lemonvox_win_target(lv_trigger)

add_executable(trigger_unit_test "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/trigger_unit_test.cpp")
target_link_libraries(trigger_unit_test PRIVATE lv_trigger)
target_include_directories(trigger_unit_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../tests")
lemonvox_win_test(trigger_unit_test)

add_executable(trigger_interactive_test "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/trigger_interactive_test.cpp")
target_link_libraries(trigger_interactive_test PRIVATE lv_trigger)
target_include_directories(trigger_interactive_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../tests")
lemonvox_win_test(trigger_interactive_test)
set_tests_properties(trigger_interactive_test PROPERTIES LABELS interactive TIMEOUT 900)
