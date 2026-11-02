add_library(lv_app_core STATIC dictation_controller.cpp)
target_link_libraries(lv_app_core PUBLIC lv_common lv_prefs lv_net ole32)
lemonvox_win_target(lv_app_core)

add_executable(controller_test "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/controller_test.cpp")
target_link_libraries(controller_test PRIVATE lv_app_core)
lemonvox_win_test(controller_test)

# Real Lemonade pipeline with synthesized speech; never records the microphone.
add_executable(pipeline_lemonade_test "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/pipeline_lemonade_test.cpp")
target_link_libraries(pipeline_lemonade_test PRIVATE lv_app_core lv_capture)
lemonvox_win_test(pipeline_lemonade_test)
set_tests_properties(pipeline_lemonade_test PROPERTIES LABELS lemonade SKIP_RETURN_CODE 77 TIMEOUT 300)
