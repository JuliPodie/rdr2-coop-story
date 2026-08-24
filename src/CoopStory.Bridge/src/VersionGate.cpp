#include "coopstory/bridge/VersionGate.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace coopstory::bridge {
namespace {

[[nodiscard]] bool EqualsAsciiCaseInsensitive(
    const std::string_view lhs,
    const std::string_view rhs) {
    return lhs.size() == rhs.size() &&
           std::equal(
               lhs.begin(),
               lhs.end(),
               rhs.begin(),
               [](const char left, const char right) {
                   return std::tolower(static_cast<unsigned char>(left)) ==
                          std::tolower(static_cast<unsigned char>(right));
               });
}

[[nodiscard]] std::string UpperAscii(std::string value) {
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const unsigned char character) {
            return static_cast<char>(std::toupper(character));
        });
    return value;
}

[[nodiscard]] VersionGateResult Deny(
    const GateFailure failure,
    std::string message) {
    return {false, failure, std::move(message)};
}

}  // namespace

VersionGateResult VersionGate::Evaluate(
    const GameIdentity& identity,
    const RuntimeMode& mode) {
    if (identity.executableName.empty() || identity.fileVersion.empty() ||
        identity.sha256.empty()) {
        return Deny(
            GateFailure::IdentityProbeFailed,
            "game identity is incomplete; bridge remains disabled");
    }
    if (!EqualsAsciiCaseInsensitive(
            identity.executableName,
            kSupportedExecutableName)) {
        return Deny(
            GateFailure::WrongExecutable,
            "current process is not the supported RDR2.exe");
    }
    if (identity.fileVersion != kSupportedFileVersion) {
        return Deny(
            GateFailure::UnsupportedVersion,
            "unsupported RDR2 file version (expected 1.0.1491.50)");
    }
    if (UpperAscii(identity.sha256) != kSupportedExecutableSha256) {
        return Deny(
            GateFailure::UnsupportedHash,
            "unsupported RDR2.exe SHA-256; no compatibility fallback is allowed");
    }
    if (mode.onlineSessionActive) {
        return Deny(
            GateFailure::OnlineSessionDetected,
            "online/network session detected; Story Mode bridge is disabled");
    }
    if (!mode.storyModeKnown) {
        return Deny(
            GateFailure::StoryModeUnverified,
            "Story Mode could not be verified; bridge fails closed");
    }
    if (!mode.isStoryMode) {
        return Deny(
            GateFailure::NotStoryMode,
            "current game state is not Story Mode");
    }
    return {true, GateFailure::None, "supported offline Story Mode build"};
}

}  // namespace coopstory::bridge
