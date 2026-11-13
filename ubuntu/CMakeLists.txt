cmake_minimum_required(VERSION 3.22)

project(LemonVoxUbuntu LANGUAGES C CXX)

include(CTest)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GLIB REQUIRED IMPORTED_TARGET glib-2.0 gio-2.0)
pkg_check_modules(SOUP REQUIRED IMPORTED_TARGET libsoup-3.0)
pkg_check_modules(GSTREAMER REQUIRED IMPORTED_TARGET gstreamer-1.0 gstreamer-app-1.0)

set(LEMONVOX_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")

add_library(lemonvox_core STATIC
    "${LEMONVOX_ROOT}/core/session.cpp"
    "${LEMONVOX_ROOT}/core/protocol.cpp"
    "${LEMONVOX_ROOT}/core/workflow.cpp"
    "${LEMONVOX_ROOT}/third_party/yyjson/yyjson.c")
target_compile_features(lemonvox_core PUBLIC c_std_17 cxx_std_17)
target_compile_options(lemonvox_core PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-Wall;-Wextra;-Werror>
    $<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Werror>)
target_compile_definitions(lemonvox_core PRIVATE YYJSON_READER_DEPTH_LIMIT=513)
target_include_directories(lemonvox_core PUBLIC "${LEMONVOX_ROOT}/core"
    PRIVATE "${LEMONVOX_ROOT}/third_party/yyjson")

add_library(lemonvox_ubuntu_adapters STATIC adapters.cpp)
target_compile_features(lemonvox_ubuntu_adapters PUBLIC cxx_std_17)
target_compile_options(lemonvox_ubuntu_adapters PRIVATE -Wall -Wextra -Werror)
target_include_directories(lemonvox_ubuntu_adapters PUBLIC "${CMAKE_CURRENT_LIST_DIR}" "${LEMONVOX_ROOT}/core")
target_link_libraries(lemonvox_ubuntu_adapters PUBLIC lemonvox_core PkgConfig::GLIB PkgConfig::SOUP PkgConfig::GSTREAMER)

if(BUILD_TESTING)
    foreach(test_name model_setup model_setup_client)
        add_executable(lemonvox_${test_name}_test "${LEMONVOX_ROOT}/tests/${test_name}_test.cpp")
        target_compile_features(lemonvox_${test_name}_test PRIVATE cxx_std_17)
        target_compile_options(lemonvox_${test_name}_test PRIVATE -UNDEBUG -Wall -Wextra -Werror)
        target_link_libraries(lemonvox_${test_name}_test PRIVATE lemonvox_core)
        add_test(NAME lemonvox_${test_name}_test COMMAND lemonvox_${test_name}_test)
    endforeach()
    add_executable(lemonvox_ubuntu_adapters_test tests/adapters_test.cpp)
    target_compile_features(lemonvox_ubuntu_adapters_test PRIVATE cxx_std_17)
    # Narrow white-box lifetime regression; production targets retain normal
    # C++ access control and expose no test-only adapter API.
    target_compile_options(lemonvox_ubuntu_adapters_test PRIVATE -UNDEBUG -fno-access-control -Wall -Wextra -Werror)
    target_link_libraries(lemonvox_ubuntu_adapters_test PRIVATE lemonvox_ubuntu_adapters)
    add_test(NAME lemonvox_ubuntu_adapters_test COMMAND lemonvox_ubuntu_adapters_test)
endif()

include("${CMAKE_CURRENT_LIST_DIR}/application.cmake")
