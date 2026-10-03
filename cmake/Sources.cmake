# Explicit compile list from ThreadGuard.vcxproj. interface.cpp comes from the
# MetaHook SDK: EXPOSE_SINGLE_INTERFACE, which exports CreateInterface, lives there.
set(THREADGUARD_SOURCES
    "${PROJECT_SOURCE_DIR}/src/ThreadManager.cpp"
    "${PROJECT_SOURCE_DIR}/src/exportfuncs.cpp"
    "${PROJECT_SOURCE_DIR}/src/plugins.cpp"
    "${PROJECT_SOURCE_DIR}/src/privatehook.cpp"
    "${METAHOOK_SOURCE_PATH}/include/HLSDK/common/interface.cpp"
)
