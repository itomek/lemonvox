add_library(lv_app_core STATIC dictation_controller.cpp file_capture.cpp)
target_link_libraries(lv_app_core PUBLIC lv_common lv_prefs lv_net lv_capture ole32)
lemonvox_win_target(lv_app_core)

add_executable(controller_test "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/controller_test.cpp")
target_link_libraries(controller_test PRIVATE lv_app_core)
lemonvox_win_test(controller_test)

# Real Lemonade pipeline with synthesized speech; never records the microphone.
add_executable(pipeline_lemonade_test "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/pipeline_lemonade_test.cpp")
target_link_libraries(pipeline_lemonade_test PRIVATE lv_app_core lv_capture)
lemonvox_win_test(pipeline_lemonade_test)
set_tests_properties(pipeline_lemonade_test PROPERTIES LABELS lemonade SKIP_RETURN_CODE 77 TIMEOUT 300)

# The tray application.
add_executable(lemonvox WIN32 main.cpp)
target_link_libraries(lemonvox PRIVATE lv_app_core lv_capture lv_insertion lv_trigger lv_ui lv_settings shell32)
lemonvox_win_target(lemonvox)
lemonvox_win_app_resources(lemonvox)
set_target_properties(lemonvox PROPERTIES OUTPUT_NAME LemonVox)
add_test(NAME app_smoke COMMAND lemonvox --smoke)
set_tests_properties(app_smoke PROPERTIES LABELS interactive TIMEOUT 60)

# Full app on the real desktop: global Alt double tap, Lemonade, real insertion.
add_executable(app_e2e_test "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/app_e2e_test.cpp")
target_link_libraries(app_e2e_test PRIVATE lv_app_core lv_insertion)
target_include_directories(app_e2e_test PRIVATE "${PROJECT_SOURCE_DIR}/tests")
lemonvox_win_target(app_e2e_test)
add_dependencies(app_e2e_test lemonvox)
add_test(NAME app_e2e_test COMMAND app_e2e_test "$<TARGET_FILE:lemonvox>")
set_tests_properties(app_e2e_test PROPERTIES LABELS "interactive;lemonade" SKIP_RETURN_CODE 77 TIMEOUT 600)
