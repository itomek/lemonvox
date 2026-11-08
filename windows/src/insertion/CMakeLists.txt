add_library(lv_insertion STATIC
    clipboard.cpp
    integrity.cpp
    target.cpp
    win_insertion.cpp)
target_link_libraries(lv_insertion PUBLIC lv_common PRIVATE ole32 oleaut32 oleacc uiautomationcore user32 advapi32)
lemonvox_win_target(lv_insertion)

set(LV_INSERTION_TESTS "${CMAKE_CURRENT_SOURCE_DIR}/../../tests")

add_executable(insertion_unit_test "${LV_INSERTION_TESTS}/insertion_unit_test.cpp")
target_link_libraries(insertion_unit_test PRIVATE lv_insertion)
target_include_directories(insertion_unit_test PRIVATE "${LV_INSERTION_TESTS}")
lemonvox_win_test(insertion_unit_test)

add_executable(insertion_interactive_test "${LV_INSERTION_TESTS}/insertion_interactive_test.cpp")
target_link_libraries(insertion_interactive_test PRIVATE lv_insertion lv_trigger)
target_include_directories(insertion_interactive_test PRIVATE "${LV_INSERTION_TESTS}")
lemonvox_win_test(insertion_interactive_test)
set_tests_properties(insertion_interactive_test PROPERTIES LABELS interactive)

# Manual matrix helper (not a ctest): drives the adapter against a window the
# caller launched. See windows/tests/insertion-matrix.md.
add_executable(insertion_matrix_probe "${LV_INSERTION_TESTS}/insertion_matrix_probe.cpp")
target_link_libraries(insertion_matrix_probe PRIVATE lv_insertion)
target_include_directories(insertion_matrix_probe PRIVATE "${LV_INSERTION_TESTS}")
lemonvox_win_target(insertion_matrix_probe)
