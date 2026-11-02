cmake_minimum_required(VERSION 3.24)
project(LemonVoxWindows VERSION 0.0.0 LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "The LemonVox Windows host builds only on Windows.")
endif()

set(LEMONVOX_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(LEMONVOX_WARNINGS /W4 /WX /permissive- /utf-8)

# Tests rely on assert(), so NDEBUG is applied per production target instead of globally.
foreach(flags_var CMAKE_C_FLAGS_RELEASE CMAKE_CXX_FLAGS_RELEASE CMAKE_C_FLAGS_RELWITHDEBINFO
        CMAKE_CXX_FLAGS_RELWITHDEBINFO CMAKE_C_FLAGS_MINSIZEREL CMAKE_CXX_FLAGS_MINSIZEREL)
    string(REPLACE "/DNDEBUG" "" ${flags_var} "${${flags_var}}")
endforeach()
set(LEMONVOX_RELEASE_DEFINES $<$<NOT:$<CONFIG:Debug>>:NDEBUG>)

add_library(yyjson STATIC "${LEMONVOX_ROOT}/third_party/yyjson/yyjson.c")
target_compile_definitions(yyjson PUBLIC YYJSON_READER_DEPTH_LIMIT=513 PRIVATE ${LEMONVOX_RELEASE_DEFINES})
target_include_directories(yyjson PUBLIC "${LEMONVOX_ROOT}/third_party/yyjson")
target_compile_options(yyjson PRIVATE /utf-8)

add_library(lemonvox_core STATIC
    "${LEMONVOX_ROOT}/core/session.cpp"
    "${LEMONVOX_ROOT}/core/protocol.cpp"
    "${LEMONVOX_ROOT}/core/workflow.cpp")
target_include_directories(lemonvox_core PUBLIC "${LEMONVOX_ROOT}/core")
target_link_libraries(lemonvox_core PUBLIC yyjson)
target_compile_options(lemonvox_core PRIVATE ${LEMONVOX_WARNINGS})
target_compile_definitions(lemonvox_core PRIVATE ${LEMONVOX_RELEASE_DEFINES})

enable_testing()
foreach(test_name session trigger_core contracts protocol workflow_core)
    add_executable(${test_name}_test "${LEMONVOX_ROOT}/tests/${test_name}_test.cpp")
    target_link_libraries(${test_name}_test PRIVATE lemonvox_core)
    # The portable tests compare C int results with bool; C4805 is MSVC-only noise there.
    target_compile_options(${test_name}_test PRIVATE ${LEMONVOX_WARNINGS} /wd4805)
    add_test(NAME core_${test_name} COMMAND ${test_name}_test)
endforeach()
