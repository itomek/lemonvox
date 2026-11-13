add_library(lv_net STATIC winhttp_transport.cpp model_catalog.cpp server_health.cpp)
target_link_libraries(lv_net PUBLIC lv_common PRIVATE winhttp)
lemonvox_win_target(lv_net)

set(LEMONVOX_WIN_TESTS "${CMAKE_CURRENT_SOURCE_DIR}/../../tests")

add_executable(net_test "${LEMONVOX_WIN_TESTS}/net_test.cpp")
target_link_libraries(net_test PRIVATE lv_net ws2_32)
lemonvox_win_test(net_test)

# Talks to a real local Lemonade server; skips (passes) when none is reachable.
add_executable(lemonade_net_test "${LEMONVOX_WIN_TESTS}/lemonade_net_test.cpp")
target_link_libraries(lemonade_net_test PRIVATE lv_net)
lemonvox_win_test(lemonade_net_test)
set_tests_properties(lemonade_net_test PROPERTIES LABELS lemonade)

add_executable(server_health_test "${LEMONVOX_WIN_TESTS}/server_health_test.cpp")
target_link_libraries(server_health_test PRIVATE lv_net)
lemonvox_win_test(server_health_test)
