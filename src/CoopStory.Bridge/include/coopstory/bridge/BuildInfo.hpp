#pragma once

#include <string_view>

// CMake supplies a UTC configure-time identifier for every bridge build.
// Keep a fallback so SDK-independent tooling can still compile the core when
// it is built outside the normal preset workflow.
#ifndef COOPSTORY_BRIDGE_BUILD_ID
#define COOPSTORY_BRIDGE_BUILD_ID "dev"
#endif

namespace coopstory::bridge {

inline constexpr std::string_view kBridgeBuildId{
    COOPSTORY_BRIDGE_BUILD_ID};

}  // namespace coopstory::bridge
