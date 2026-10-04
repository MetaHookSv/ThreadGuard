cmake_minimum_required(VERSION 3.21)
cmake_policy(SET CMP0091 NEW)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
project(ThreadGuard LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "ThreadGuard requires Windows MSVC x86. Configure with -A Win32.")
endif()
get_property(multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(multi_config)
    set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "Supported configurations" FORCE)
elseif(NOT CMAKE_BUILD_TYPE MATCHES "^(Debug|Release)$")
    message(FATAL_ERROR "Select Debug or Release.")
endif()

include(cmake/Dependencies.cmake)
threadguard_prepare_dependencies()
include("${VC_LTL_Root}/VC-LTL helper for cmake.cmake")

# Include directories and definitions follow ThreadGuard.vcxproj. The plugin
# links no third-party library and, since it no longer decodes instructions,
# resolves no Capstone headers either.
add_library(threadguard_settings INTERFACE)
target_include_directories(threadguard_settings INTERFACE
    src
    "${METAHOOK_SOURCE_PATH}/include"
    "${METAHOOK_SOURCE_PATH}/include/HLSDK/cl_dll"
    "${METAHOOK_SOURCE_PATH}/include/HLSDK/common"
    "${METAHOOK_SOURCE_PATH}/include/HLSDK/engine"
    "${METAHOOK_SOURCE_PATH}/include/HLSDK/pm_shared"
    "${METAHOOK_SOURCE_PATH}/include/HLSDK/public"
    "${METAHOOK_SOURCE_PATH}/include/SourceSDK"
    "${METAHOOK_SOURCE_PATH}/include/SourceSDK/tier0"
    "${METAHOOK_SOURCE_PATH}/include/SourceSDK/tier1"
    "${METAHOOK_SOURCE_PATH}/include/SourceSDK/tier3"
    "${METAHOOK_SOURCE_PATH}/include/SourceSDK/vstdlib"
    "${METAHOOK_SOURCE_PATH}/include/Interface"
)
# _MBCS mirrors the original MultiByte character set.
target_compile_definitions(threadguard_settings INTERFACE
    WIN32 _WINDOWS _USRDLL _MBCS NO_MALLOC_OVERRIDE
    $<$<CONFIG:Debug>:_DEBUG>
    $<$<CONFIG:Release>:NDEBUG>)
# Debug used to turn every warning off, Release stayed at Level3 with the SDK's
# known warnings suppressed.
target_compile_options(threadguard_settings INTERFACE /permissive
    $<$<CONFIG:Debug>:/W0>
    $<$<CONFIG:Release>:/W3>
    /wd4311 /wd4312 /wd4819 /wd4996)

include(cmake/Sources.cmake)
add_library(ThreadGuard SHARED ${THREADGUARD_SOURCES})
target_link_libraries(ThreadGuard PRIVATE threadguard_settings)
target_compile_options(ThreadGuard PRIVATE /MP /Zi)
target_link_options(ThreadGuard PRIVATE /DEBUG /SUBSYSTEM:WINDOWS
    $<$<CONFIG:Debug>:/INCREMENTAL>
    $<$<CONFIG:Release>:/INCREMENTAL:NO>
    $<$<CONFIG:Release>:/OPT:REF>
    $<$<CONFIG:Release>:/OPT:ICF>)
set_target_properties(ThreadGuard PROPERTIES INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)

install(TARGETS ThreadGuard RUNTIME DESTINATION svencoop/metahook/plugins)
install(FILES "$<TARGET_PDB_FILE:ThreadGuard>" DESTINATION svencoop/metahook/plugins)

# ThreadGuard ships its own gamedata catalog, pruned to the single global it
# resolves, in a nested directory the launcher's catalog loader merges
# (metahook/gamedata/threadguard). The manifest lists that symbol; the
# synchronizer downloads the upstream catalog into a persistent cache, prunes
# it, and publishes the result.
option(THREADGUARD_SYNC_GAMEDATA "Download and prune ThreadGuard's gamedata catalog before building" ON)
set(THREADGUARD_GAMEDATA_DIR "${PROJECT_BINARY_DIR}/assets/svencoop/metahook/gamedata/threadguard" CACHE PATH "ThreadGuard gamedata catalog directory")
if(THREADGUARD_SYNC_GAMEDATA)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_custom_target(ThreadGuardGameData
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/sync-gamedata.py"
            --manifest "${CMAKE_CURRENT_SOURCE_DIR}/scripts/manifests/threadguard.json"
            --target-dir "${THREADGUARD_GAMEDATA_DIR}" --temp-root "${PROJECT_BINARY_DIR}/gamedata-sync"
        COMMENT "Synchronizing and validating ThreadGuard gamedata"
        VERBATIM)
    add_custom_target(ThreadGuardGameDataValidate
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/validate-gamedata.py"
            "${THREADGUARD_GAMEDATA_DIR}" --manifest "${CMAKE_CURRENT_SOURCE_DIR}/scripts/manifests/threadguard.json"
        COMMENT "Validating ThreadGuard gamedata against the manifest"
        VERBATIM)
    add_dependencies(ThreadGuardGameDataValidate ThreadGuardGameData)
    add_dependencies(ThreadGuard ThreadGuardGameDataValidate)
endif()
install(DIRECTORY "${THREADGUARD_GAMEDATA_DIR}/" DESTINATION svencoop/metahook/gamedata/threadguard OPTIONAL)

# The aggregate solution owns its single LaunchGame target.
if(CMAKE_SOURCE_DIR STREQUAL PROJECT_SOURCE_DIR)
    option(METAHOOKSV_ENABLE_LAUNCH_GAME "Add the Visual Studio build/deploy/debug target" OFF)
    if(METAHOOKSV_ENABLE_LAUNCH_GAME)
        include(cmake/LaunchGame.cmake)
        metahooksv_add_launch_game()
    endif()
endif()
