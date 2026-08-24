cmake_minimum_required(VERSION 3.25)

project(CoopStoryBridge VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "CoopStory.Bridge supports Windows only.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "CoopStory.Bridge must be configured with an x64 toolchain.")
endif()

option(COOPSTORY_BUILD_SIMULATOR "Build the SDK-free bridge simulator" ON)
option(COOPSTORY_BUILD_SELF_TESTS "Build dependency-free bridge self-tests" ON)
option(COOPSTORY_BUILD_ASI "Build the ScriptHookRDR2 .asi plugin" OFF)
option(
    COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
    "Enable RDR2 native hashes that must first be checked against the installed SDK"
    OFF
)
set(
    SCRIPT_HOOK_RDR2_SDK_DIR
    ""
    CACHE PATH
    "Path to the extracted official ScriptHookRDR2 SDK"
)

add_library(CoopStory.Bridge.Core STATIC
    src/AnimationReplicationCodec.cpp
    src/BridgeRuntime.cpp
    src/EntityRegistry.cpp
    src/FrameCodec.cpp
    src/MenuController.cpp
    src/SessionMenuController.cpp
    src/MissionBubble.cpp
    src/NamedPipeClient.cpp
    src/PlayerRuntime.cpp
    src/RemoteMotion.cpp
    src/Telemetry.cpp
    src/VersionGate.cpp
    src/Win32GameIdentity.cpp
    src/WorldMirror.cpp
)

target_include_directories(CoopStory.Bridge.Core
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
target_compile_features(CoopStory.Bridge.Core PUBLIC cxx_std_20)
target_compile_definitions(CoopStory.Bridge.Core
    PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0A00
        UNICODE
        _UNICODE
)
target_link_libraries(CoopStory.Bridge.Core
    PUBLIC
        advapi32
        bcrypt
        version
)

if(MSVC)
    target_compile_options(CoopStory.Bridge.Core PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(CoopStory.Bridge.Core PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(COOPSTORY_BUILD_SIMULATOR)
    add_executable(CoopStory.Bridge.Sim
        sim/SimulatedScriptHookFacade.cpp
        sim/SimulationMain.cpp
    )
    target_link_libraries(CoopStory.Bridge.Sim PRIVATE CoopStory.Bridge.Core)
    target_compile_features(CoopStory.Bridge.Sim PRIVATE cxx_std_20)
endif()

if(COOPSTORY_BUILD_ASI)
    if(SCRIPT_HOOK_RDR2_SDK_DIR STREQUAL "")
        message(FATAL_ERROR
            "COOPSTORY_BUILD_ASI=ON requires SCRIPT_HOOK_RDR2_SDK_DIR. "
            "Point it at the extracted official SDK; the SDK is not vendored."
        )
    endif()

    find_path(COOPSTORY_SCRIPT_HOOK_INCLUDE_DIR
        NAMES main.h
        PATHS
            "${SCRIPT_HOOK_RDR2_SDK_DIR}"
            "${SCRIPT_HOOK_RDR2_SDK_DIR}/inc"
            "${SCRIPT_HOOK_RDR2_SDK_DIR}/include"
        NO_DEFAULT_PATH
    )
    if(NOT COOPSTORY_SCRIPT_HOOK_INCLUDE_DIR)
        message(FATAL_ERROR
            "main.h was not found below SCRIPT_HOOK_RDR2_SDK_DIR='"
            "${SCRIPT_HOOK_RDR2_SDK_DIR}'. Extract the official SDK first."
        )
    endif()
    find_library(COOPSTORY_SCRIPT_HOOK_LIBRARY
        NAMES ScriptHookRDR2
        PATHS
            "${SCRIPT_HOOK_RDR2_SDK_DIR}"
            "${SCRIPT_HOOK_RDR2_SDK_DIR}/lib"
        NO_DEFAULT_PATH
    )
    if(NOT COOPSTORY_SCRIPT_HOOK_LIBRARY)
        message(FATAL_ERROR
            "ScriptHookRDR2.lib was not found below SCRIPT_HOOK_RDR2_SDK_DIR='"
            "${SCRIPT_HOOK_RDR2_SDK_DIR}'. Extract the complete official SDK."
        )
    endif()

    add_library(CoopStory.Bridge SHARED
        sdk/AsiEntry.cpp
        sdk/ScriptHookSdkFacade.cpp
    )
    target_link_libraries(CoopStory.Bridge
        PRIVATE
            CoopStory.Bridge.Core
            "${COOPSTORY_SCRIPT_HOOK_LIBRARY}"
    )
    target_include_directories(CoopStory.Bridge
        PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/sdk"
    )
    # Treat the externally supplied SDK as a system include so warnings from
    # its legacy headers do not obscure warnings in our own ASI sources.
    target_include_directories(CoopStory.Bridge
        SYSTEM PRIVATE
            "${COOPSTORY_SCRIPT_HOOK_INCLUDE_DIR}"
    )
    target_compile_features(CoopStory.Bridge PRIVATE cxx_std_20)
    target_compile_definitions(CoopStory.Bridge PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0A00
        UNICODE
        _UNICODE
        COOPSTORY_HAS_SCRIPT_HOOK=1
        COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS=$<BOOL:${COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS}>
    )
    set_target_properties(CoopStory.Bridge PROPERTIES
        PREFIX ""
        SUFFIX ".asi"
        OUTPUT_NAME "CoopStoryBridge"
    )
    if(MSVC)
        target_compile_options(CoopStory.Bridge PRIVATE /W4 /permissive- /EHsc)
    endif()
endif()

if(COOPSTORY_BUILD_SELF_TESTS)
    enable_testing()
    add_subdirectory(
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/Bridge.SelfTest"
        "${CMAKE_CURRENT_BINARY_DIR}/Bridge.SelfTest"
    )
endif()
