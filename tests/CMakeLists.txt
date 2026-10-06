cmake_minimum_required(VERSION 3.21)
cmake_policy(SET CMP0091 NEW)
project(ThreadGuardTests LANGUAGES CXX)
if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "ThreadGuard tests require Windows MSVC x86 (-A Win32).")
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(METAHOOK_SOURCE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../../MetaHook" CACHE PATH "Local MetaHook SDK")
if(NOT EXISTS "${METAHOOK_SOURCE_PATH}/include/metahook.h")
    message(FATAL_ERROR "Set METAHOOK_SOURCE_PATH to the MetaHook SDK checkout.")
endif()
enable_testing()
add_executable(network_thread_state_test network_thread_state_test.cpp)
add_executable(network_hooks_test network_hooks_test.cpp)
target_include_directories(network_hooks_test PRIVATE
    "${METAHOOK_SOURCE_PATH}/include"
    "${METAHOOK_SOURCE_PATH}/include/HLSDK/common"
    "${METAHOOK_SOURCE_PATH}/include/HLSDK/engine"
    "${METAHOOK_SOURCE_PATH}/include/HLSDK/cl_dll"
    "${METAHOOK_SOURCE_PATH}/include/HLSDK/public"
    "${METAHOOK_SOURCE_PATH}/include/HLSDK/pm_shared"
    "${METAHOOK_SOURCE_PATH}/include/Interface"
    "${METAHOOK_SOURCE_PATH}/include/SourceSDK"
    "${METAHOOK_SOURCE_PATH}/include/SourceSDK/tier0"
    "${METAHOOK_SOURCE_PATH}/include/SourceSDK/tier1")
target_link_libraries(network_hooks_test PRIVATE ws2_32)
add_test(NAME network_thread_state COMMAND network_thread_state_test)
add_test(NAME network_handlers COMMAND network_hooks_test)
set_tests_properties(network_thread_state network_handlers PROPERTIES TIMEOUT 30)
