#pragma once

#include <string>
#include <string_view>

namespace coopstory::bridge {

// Checks that the Bridge is attached to the tested RDR2 Story Mode build.
// It refuses unknown/Online builds instead of issuing multiplayer native calls.
inline constexpr std::string_view kSupportedExecutableName = "RDR2.exe";
inline constexpr std::string_view kSupportedFileVersion = "1.0.1491.50";
inline constexpr std::string_view kSupportedExecutableSha256 =
    "B56C9548F670654A9B73BF25DEF3CD73AF12E269F6E47DBA28A34079ADAF465E";

struct GameIdentity final {
    std::string executableName{};
    std::string fileVersion{};
    std::string sha256{};
};

struct RuntimeMode final {
    bool storyModeKnown{};
    bool isStoryMode{};
    bool onlineSessionActive{};
};

enum class GateFailure {
    None,
    IdentityProbeFailed,
    WrongExecutable,
    UnsupportedVersion,
    UnsupportedHash,
    StoryModeUnverified,
    NotStoryMode,
    OnlineSessionDetected,
};

struct VersionGateResult final {
    bool allowed{};
    GateFailure failure{GateFailure::None};
    std::string message{};
};

class VersionGate final {
public:
    [[nodiscard]] static VersionGateResult Evaluate(
        const GameIdentity& identity,
        const RuntimeMode& mode);
};

struct GameIdentityProbeResult final {
    bool succeeded{};
    GameIdentity identity{};
    std::string error{};
};

[[nodiscard]] GameIdentityProbeResult ProbeCurrentGameIdentity();

}  // namespace coopstory::bridge
