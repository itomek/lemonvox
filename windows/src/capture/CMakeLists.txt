add_library(lv_capture STATIC
    wav.cpp
    wasapi_stream.cpp
    wasapi_capture.cpp
    device_list.cpp)
target_link_libraries(lv_capture PUBLIC lv_common PRIVATE ole32)
lemonvox_win_target(lv_capture)

set(LV_CAPTURE_TESTS "${CMAKE_CURRENT_SOURCE_DIR}/../../tests")

add_executable(capture_test "${LV_CAPTURE_TESTS}/capture_test.cpp")
target_link_libraries(capture_test PRIVATE lv_capture)
lemonvox_win_test(capture_test)

# Enumerates endpoints and initializes (never starts) the default capture
# client. Exits 77 (skipped) without a usable device or microphone access.
add_executable(capture_device_test "${LV_CAPTURE_TESTS}/capture_device_test.cpp")
target_link_libraries(capture_device_test PRIVATE lv_capture)
lemonvox_win_test(capture_device_test)
set_tests_properties(capture_device_test PROPERTIES LABELS audio-device SKIP_RETURN_CODE 77)

# Opt-in manual tool; records only with an explicit --seconds N and is never
# registered with ctest.
add_executable(lemonvox_mic_probe mic_probe.cpp)
target_link_libraries(lemonvox_mic_probe PRIVATE lv_capture)
lemonvox_win_target(lemonvox_mic_probe)
