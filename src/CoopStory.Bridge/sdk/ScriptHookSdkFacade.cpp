#include "ScriptHookSdkFacade.hpp"
#include "coopstory/bridge/CampaignMissionCatalog.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <main.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <numbers>
#include <optional>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
#include <natives.h>
#endif

namespace coopstory::bridge::sdk {
namespace {

constexpr std::uintmax_t kBridgeLogMaxBytes = 8U * 1024U * 1024U;
constexpr unsigned int kBridgeLogArchiveCount = 3U;

// Shared capabilities are intentionally opt-in.  A valid wire record is not
// enough authority to alter a Story save: each native mapping has to be
// manually proven against this game build first.  Add records here only with a
// matching guarded in-game probe and automated protocol coverage.
constexpr std::uint32_t kRepeatingShotgunWeaponHash = 1'674'213'418U;
constexpr std::uint32_t kPoisonThrowingKnifePamphletHash = 0x366089E7U;

[[nodiscard]] bool IsSupportedCampaignCapability(
    const CampaignCapabilityPayload& capability) noexcept {
    switch (capability.kind) {
        case CampaignCapabilityKind::WeaponShopEligibility:
            return capability.recordHash == kRepeatingShotgunWeaponHash;
        case CampaignCapabilityKind::Recipe:
            return capability.recordHash == kPoisonThrowingKnifePamphletHash;
        case CampaignCapabilityKind::CapacityUpgrade:
        case CampaignCapabilityKind::ActivityGate:
            return false;
    }
    return false;
}

#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
// The bundled 1207 SDK exposes the UNLOCK family but omits the name of this
// read-only resolver. rdr3-natives documents 0x865F36299079FB75 as
// _GET_WEAPON_UNLOCK(Hash weaponHash) -> Hash. Keep it local instead of
// editing vendored SDK headers.
[[nodiscard]] Hash GetWeaponUnlock(const Hash weaponHash) {
    return ::invoke<Hash>(0x865F36299079FB75ULL, weaponHash);
}
#endif
// A movement task contains a fixed destination. Updating it only after three
// metres made a walking replica reach an old point, stop, and then sprint to
// the next one. Route intent can update roughly three times per second without
// clearing the task graph, so the destination remains ahead at sprint speed.
constexpr std::uint64_t kRemoteTaskRefreshMilliseconds = 2'000U;
constexpr std::uint64_t kRemoteTaskMinimumRefreshMilliseconds = 300U;
constexpr std::uint64_t kRemoteMotionDiagnosticsMilliseconds = 5'000U;
constexpr float kRemoteTaskHeadingRefreshDegrees = 35.0F;
constexpr float kRemoteTaskDestinationRefreshMeters = 0.35F;
constexpr float kRemoteTaskRecoveryMeters = 10.0F;
constexpr std::uint64_t kRemoteTaskRecoveryCooldownMilliseconds = 4'000U;
constexpr std::uint64_t kRemoteTaskWatchdogToleranceMilliseconds = 250U;
constexpr std::uint64_t kRemoteTaskWatchdogInitialCooldownMilliseconds = 500U;
constexpr std::uint64_t kRemoteTaskWatchdogMaximumCooldownMilliseconds = 1'500U;
constexpr int kRemoteTaskTimeoutMilliseconds = 12'000;
constexpr int kRemoteIdleTaskMilliseconds = 1'000;
constexpr float kRemoteTaskStoppingRangeMeters = 0.15F;
constexpr std::size_t kWorldPedPoolCapacity = 1'024U;
constexpr std::uint64_t kWorldDamageIntentMinimumIntervalMilliseconds = 90U;
constexpr float kWorldDamageIntentFixedDamage = 25.0F;
// Ambient population remains bounded to the runtime's normal 80 m bubble.
// Mission parties regularly spread farther apart while riding, so actors the
// host script owns stay observable across the complete soft-bubble instead of
// disappearing just when the guest falls behind.
constexpr float kMissionScriptOwnedActorRadiusMeters = 300.0F;
constexpr std::uint64_t kWorldModelLoadTimeoutMilliseconds = 5'000U;
constexpr std::uint64_t kWorldAimTaskRefreshMilliseconds = 250U;
constexpr float kWorldProxySnapDistanceMeters = 6.0F;
constexpr std::uint64_t kRemotePlayerAimTaskRefreshMilliseconds = 200U;
constexpr int kRemotePlayerAimTaskDurationMilliseconds = 350;
constexpr float kRemotePlayerAimTargetRefreshMeters = 0.35F;
constexpr std::uint64_t kRemotePlayerAimTargetCacheMilliseconds = 750U;
constexpr std::uint64_t kRemotePlayerFireLatchMilliseconds = 500U;
constexpr std::uint64_t kRemoteCoverHoldRefreshMilliseconds = 750U;
constexpr std::uint64_t kRemoteCoverAcquireRetryMilliseconds = 350U;
constexpr std::uint64_t kRemoteCoverFallbackMilliseconds = 300U;
constexpr std::uint8_t kRemoteCoverMaximumAcquireRetries = 2U;
constexpr std::uint64_t kRemoteCoverLostRecoveryMilliseconds = 450U;
constexpr std::uint64_t kRemoteCoverRecoveryCooldownMilliseconds = 1'200U;
constexpr std::uint64_t kRemoteCoverFallbackRecoveryMilliseconds = 250U;
constexpr std::uint64_t kRemoteCrouchMissingRecoveryMilliseconds = 250U;
constexpr std::uint64_t kRemoteCrouchRecoveryCooldownMilliseconds = 500U;
constexpr std::uint64_t kLocalCoverSemanticHoldMilliseconds = 500U;
constexpr std::uint64_t kAnimGraphMissingLocomotionRecoveryMilliseconds =
    350U;
constexpr std::uint64_t kAnimGraphLocomotionRecoveryCooldownMilliseconds =
    2'000U;
constexpr float kRemoteMoveRateRisePerSecond = 0.75F;
constexpr float kRemoteMoveRateFallPerSecond = 1.20F;
constexpr std::uint64_t kRemoteWeaponGrantRetryMilliseconds = 5'000U;
constexpr std::uint64_t kRemoteWeaponVisualRefreshMilliseconds = 200U;
constexpr std::uint64_t kLocalKnownMountContinuityMilliseconds = 30'000U;
// Scripted zero-damage bullets do not consistently emit a weapon report in
// Story Mode. This built-in one-shot is used only as a spatial audio fallback
// and is attached to the remote ped; no external audio asset is distributed.
char kRemoteGunshotSoundName[] = "CarbineShotDistant";
char kRemoteGunshotSoundSet[] = "REFC_Sounds";
// A melee click is a discrete action edge, not a level that may stay true for
// several seconds while the button is held. Keep the visual pulse alive long
// enough for RDR2 to finish one strike, but never refresh it from PRESSED.
constexpr std::uint64_t kLocalMeleeAttackLatchMilliseconds = 550U;
constexpr std::uint64_t kLocalMeleeStateReleaseGraceMilliseconds = 180U;
// The local proxy can enter ragdoll after the visible strike task has already
// finished. Keep a short cause/effect window so that transition is still
// published as one victim-owned knockdown instead of being lost between two
// 20 Hz samples.
constexpr std::uint64_t kLocalPeerCombatEffectMemoryMilliseconds = 1'800U;
constexpr std::uint64_t kLocalPeerKnockdownLatchMilliseconds = 1'800U;
constexpr std::uint64_t kLocalPeerMountPullLatchMilliseconds = 550U;
constexpr float kPeerMountPullMaximumDistanceMeters = 3.5F;
// TASK_PUT_PED_DIRECTLY_INTO_MELEE is not a deterministic animation API: it
// lets local AI choose a combo. We invoke it once per reliable attack Begin
// and bound ownership tightly, so it can show one strike but can never refresh
// into a local combo or survive the matching End.
constexpr std::uint64_t kRemoteMeleeVisualMaximumMilliseconds = 1'150U;
constexpr std::uint64_t kRemotePeerDismountRetryMilliseconds = 650U;
constexpr std::uint64_t kRemotePeerDismountTimeoutMilliseconds = 2'500U;
// The local rope may need several streamed frames before it physically reaches
// the peer proxy. Announce the throw first, then upgrade the same transaction
// to a physical target effect once the local engine reports restraint.
constexpr std::uint64_t kLocalPeerLassoIntentLatchMilliseconds = 1'800U;
constexpr std::uint64_t kPeerCombatIsolationHoldMilliseconds = 2'500U;
constexpr float kDownedLethalGuardHealthFraction = 0.05F;
constexpr float kGuestLocalHazardGuardHealthFraction = 0.30F;
constexpr float kGuestLocalHazardRestoreHealthFraction = 0.75F;
constexpr std::uint64_t kReliablePlayerActionTimeoutMilliseconds = 2'500U;
constexpr std::uint64_t kPeerKnockdownRagdollRefreshMilliseconds = 250U;
constexpr int kPeerKnockdownRagdollDurationMilliseconds = 1'800;
// Sustain packets refresh the authoritative lasso snapshot every ~500 ms. If
// the terminal packet or its sidecar disappears, this lease is the final local
// guarantee that neither real player nor proxy can remain ragdolled forever.
constexpr std::uint64_t kAuthoritativeLassoLeaseMilliseconds = 3'500U;
constexpr std::uint64_t kAuthoritativeRestraintRagdollRefreshMilliseconds =
    700U;
constexpr int kAuthoritativeRestraintRagdollDurationMilliseconds = 1'250;
constexpr float kPeerLassoMaximumDistanceMeters = 15.0F;
constexpr float kPeerLassoAimCorridorMeters = 0.85F;
constexpr std::uint64_t kFrontendPauseToggleTimeoutMilliseconds = 750U;
constexpr std::uint64_t kGuestMissionQuarantineMinimumMilliseconds = 2'500U;
constexpr std::uint64_t kGuestMissionClearConfirmationMilliseconds = 750U;
constexpr std::uint64_t kGuestMissionIsolationDiagnosticsMilliseconds = 5'000U;
constexpr std::uint64_t kGuestMissionAnimSceneProbeBurstMilliseconds = 5'000U;
constexpr std::uint64_t kGuestMissionAnimScenePeriodicProbeMilliseconds = 100U;
constexpr float kMissionSpectatorPopulationRadiusMeters = 100.0F;
// Reserve local Story interactions before the guest reaches the yellow
// activation volume.  Eight metres was late enough for a queued local scene
// to start underneath the host presentation in the V29 two-PC trace.
constexpr float kGuestStoryInteractionGuardRadiusMeters = 20.0F;
constexpr std::uint64_t kWorldMirrorDiagnosticsMilliseconds = 5'000U;
constexpr float kEntityDivergenceThresholdMeters = 1.5F;
constexpr std::uint64_t kOwnedMountScanMilliseconds = 1'000U;
constexpr float kLocalOwnMountInteractionExclusionMeters = 4.5F;
constexpr std::uint64_t kRemoteMountTaskRefreshMilliseconds = 650U;
constexpr std::uint64_t kRemoteMountTaskMinimumRefreshMilliseconds = 200U;
constexpr std::uint64_t kRemoteMountRelationRetryMilliseconds = 250U;
constexpr float kRemoteMountTaskDestinationRefreshMeters = 0.90F;
constexpr float kRemoteMountMovingCorrectionMeters = 0.55F;
constexpr float kRemoteMountIdleCorrectionMeters = 0.10F;
constexpr float kRemoteMountHardCorrectionMeters = 6.0F;
constexpr std::uint64_t kWorldSemanticTaskRefreshMilliseconds = 900U;
constexpr float kWorldSemanticTaskDestinationRefreshMeters = 2.0F;
constexpr float kFallbackAimDistanceMeters = 250.0F;
constexpr int kInputGroupGameplay = 0;
constexpr int kInputFrontendPause =
    static_cast<int>(0xD82E0BD2U);
constexpr int kInputFrontendPauseAlternate =
    static_cast<int>(0x4A903C11U);
constexpr int kInputJump = static_cast<int>(0xD9D0E1C0U);
constexpr int kInputAttack = static_cast<int>(0x07CE1E61U);
constexpr int kInputMeleeAttack = static_cast<int>(0xB2F377E8U);
constexpr int kInputMeleeBlock = static_cast<int>(0xB5EEEFB7U);
constexpr int kInputMeleeGrapple = static_cast<int>(0x2277FAE9U);
constexpr int kInputMeleeGrappleAttack = static_cast<int>(0xADEAF48CU);
constexpr int kInputDuck = static_cast<int>(0xDB096B85U);
constexpr int kInputSkipCutscene = static_cast<int>(0xCDC4E4E9U);
constexpr std::uint64_t kMissionInitialCameraGraceMilliseconds = 1'500U;
constexpr std::uint64_t kMissionCameraGapHoldMilliseconds = 2'500U;
constexpr std::uint64_t kMissionCameraRenderAssertMilliseconds = 250U;
constexpr std::uint64_t kMissionResumeStreamingWarmupMilliseconds = 250U;
constexpr std::uint64_t kMissionResumeFallbackMilliseconds = 5'000U;
constexpr float kMissionCinematicStreamingRadiusMeters = 120.0F;
// The guest's hidden physical Story actor must not remain inside the same
// proximity trigger that starts the host mission. Keeping it frozen in place
// allowed the guest's private Story VM to arm during the host presentation
// and play the same cutscene two minutes later. The rendered camera keeps its
// own focus at the host scene while the invisible actor is staged vertically.
constexpr float kMissionSpectatorStoryTriggerSeparationMeters = 180.0F;
constexpr int kAnimSceneMaximumProbeHandle = 4'096;
constexpr int kAnimSceneProbeBatchSize = 128;
constexpr float kAnimSceneDurationToleranceSeconds = 0.35F;
constexpr float kAnimScenePhasePauseThreshold = 0.055F;
constexpr float kAnimScenePhaseCatchUpWindowSeconds = 0.75F;
constexpr std::uint64_t kAnimSceneProbeLogMilliseconds = 1'000U;
constexpr std::size_t kMetaPedMaximumShopComponents = 64U;
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
// These RVAs were derived offline from the known ScriptHookRDR2 1.0.1491.17
// file whose recorded SHA-256 starts 3AC29FBE and ends BF972A4C. Runtime does
// not hash the on-disk DLL: it validates the loaded PE timestamp/image size,
// exact export RVAs and known export stubs below. The inspector is read-only;
// nativeCall is never executed and no code or registration entry is patched.
// Any loaded-layout mismatch disables inspection instead of guessing.
constexpr std::uint32_t kPinnedScriptHookPeTimestamp = 0x63E4CB9AU;
constexpr std::uint32_t kPinnedScriptHookImageSize = 0x00035000U;
constexpr std::uintptr_t kPinnedScriptHookNativeInitRva = 0x00007670U;
constexpr std::uintptr_t kPinnedScriptHookNativeCallRva = 0x000076A0U;
constexpr std::uintptr_t kPinnedScriptHookHandleBaseRva = 0x000076F0U;
constexpr std::uintptr_t kPinnedScriptHookHandlerSlotRva = 0x0002E7E0U;
constexpr std::size_t kAnimSceneHandlerPrologueBytes = 32U;

struct AnimSceneNativeInspectionTarget final {
    std::uint64_t hash{};
    std::string_view name{};
    std::uintptr_t expectedRva{};
    bool signatureConfirmed{};
};

constexpr std::array<AnimSceneNativeInspectionTarget, 7U>
    kAnimSceneNativeInspectionTargets{{
        {0x1FCA98E33C1437B3ULL, "CREATE_ANIM_SCENE", 0xF5F6C4U, true},
        {0x8B720AD451CA2AB3ULL, "SET_ANIM_SCENE_ENTITY", 0xF63E54U, true},
        {0x2BF96692C67F3E53ULL, "REMOVE_ANIM_SCENE_ENTITY", 0xF63998U, true},
        {0xAB5E7CAB074D6B84ULL, "SET_ANIM_SCENE_PLAYBACK_LIST", 0xF63EE8U, true},
        {0x8A8208AE92BF87A5ULL, "LOAD_ANIM_SCENE", 0xF5ED54U, true},
        {0xF4D94AF761768700ULL, "START_ANIM_SCENE", 0xF651FCU, true},
        {0x84EEDB2C6E650000ULL, "DELETE_ANIM_SCENE", 0xF60AE8U, true},
    }};

enum class AnimSceneCaptureHookKind : std::size_t {
    Create,
    SetEntity,
    RemoveEntity,
    PlaybackList,
    Load,
    Start,
    Delete,
    Count,
};

struct NativeHandlerContextView final {
    std::uint64_t* returnValues{};
    std::uint32_t argumentCount{};
    std::uint32_t dataCount{};
    std::uint64_t* arguments{};
};

static_assert(offsetof(NativeHandlerContextView, arguments) == 0x10U);

using NativeHandlerFunction = void(__fastcall*)(NativeHandlerContextView*);

struct AnimSceneInlineHook final {
    std::uint8_t* target{};
    NativeHandlerFunction replacement{};
    NativeHandlerFunction trampoline{};
    std::array<std::uint8_t, 24U> original{};
    std::size_t overwriteLength{};
    void* trampolineMemory{};
    bool installed{};
};

struct RawCapturedAnimScene final {
    std::uint64_t captureSequence{};
    int sceneHandle{};
    std::uint32_t sceneFlags{};
    std::uint8_t createOptionFlags{};
    std::string resourceName{};
    std::string playbackList{};
    std::vector<CapturedAnimSceneRoleBinding> roles{};
    std::size_t rolesAtLoad{};
    bool loadRequested{};
    bool roleOverflow{};
    bool complete{};
};

class StoryVmAnimSceneCapture final {
public:
    void OnCreate(
        const int scene,
        const std::uint32_t sceneFlags,
        const std::uint8_t createOptionFlags,
        std::string resource,
        std::string playback) {
        if (scene <= 0 || resource.empty()) {
            return;
        }
        std::scoped_lock lock{sync_};
        scenes_[scene] = RawCapturedAnimScene{
            0U,
            scene,
            sceneFlags,
            createOptionFlags,
            std::move(resource),
            std::move(playback)};
    }

    void OnSetEntity(
        const int scene,
        std::string role,
        const LocalEntityHandle entity,
        const std::uint32_t bindingFlags) {
        if (scene <= 0 || role.empty() || entity == 0) {
            return;
        }
        std::scoped_lock lock{sync_};
        const auto found = scenes_.find(scene);
        if (found == scenes_.end()) {
            return;
        }
        auto& roles = found->second.roles;
        const auto existing = std::find_if(
            roles.begin(),
            roles.end(),
            [&role](const auto& candidate) {
                return candidate.roleName == role;
            });
        CapturedAnimSceneRoleBinding binding;
        binding.roleName = std::move(role);
        binding.localHandle = entity;
        binding.bindingFlags = bindingFlags;
        binding.flags = static_cast<std::uint16_t>(
            AnimSceneRoleFlag::Required);
        if (existing == roles.end()) {
            if (roles.size() >= kMaximumAnimSceneDefinitionRoles) {
                // Never silently publish an incomplete exact scene: a missing
                // actor binding is less safe than the established fallback.
                found->second.roleOverflow = true;
                return;
            }
            roles.push_back(std::move(binding));
        } else {
            *existing = std::move(binding);
        }
    }

    void OnRemoveEntity(const int scene, const std::string_view role) {
        if (scene <= 0 || role.empty()) {
            return;
        }
        std::scoped_lock lock{sync_};
        const auto found = scenes_.find(scene);
        if (found == scenes_.end()) {
            return;
        }
        std::erase_if(
            found->second.roles,
            [role](const auto& candidate) {
                return candidate.roleName == role;
            });
    }

    void OnPlaybackList(const int scene, std::string playback) {
        if (scene <= 0 || playback.empty()) {
            return;
        }
        std::scoped_lock lock{sync_};
        const auto found = scenes_.find(scene);
        if (found != scenes_.end()) {
            found->second.playbackList = std::move(playback);
        }
    }

    void OnLoad(const int scene) {
        std::scoped_lock lock{sync_};
        const auto found = scenes_.find(scene);
        if (found != scenes_.end()) {
            found->second.rolesAtLoad = found->second.roles.size();
            found->second.loadRequested = true;
        }
    }

    void OnStart(const int scene) {
        std::scoped_lock lock{sync_};
        const auto found = scenes_.find(scene);
        if (found == scenes_.end() || found->second.resourceName.empty() ||
            found->second.roleOverflow) {
            return;
        }
        auto completed = found->second;
        completed.captureSequence = NextSequence();
        completed.complete = true;
        found->second.complete = true;
        completed_.push_back(std::move(completed));
        while (completed_.size() > 32U) {
            completed_.pop_front();
        }
    }

    void OnDelete(const int scene) {
        std::scoped_lock lock{sync_};
        scenes_.erase(scene);
    }

    [[nodiscard]] std::vector<RawCapturedAnimScene> Drain() {
        std::scoped_lock lock{sync_};
        std::vector<RawCapturedAnimScene> drained;
        drained.reserve(completed_.size());
        while (!completed_.empty()) {
            drained.push_back(std::move(completed_.front()));
            completed_.pop_front();
        }
        return drained;
    }

    [[nodiscard]] std::vector<int> StartedSceneHandles() noexcept {
        try {
            std::scoped_lock lock{sync_};
            std::vector<int> handles;
            handles.reserve(completed_.size());
            for (const auto& completed : completed_) {
                if (completed.sceneHandle <= 0 ||
                    !scenes_.contains(completed.sceneHandle) ||
                    std::find(
                        handles.begin(),
                        handles.end(),
                        completed.sceneHandle) != handles.end()) {
                    continue;
                }
                handles.push_back(completed.sceneHandle);
            }
            return handles;
        } catch (...) {
            return {};
        }
    }

    [[nodiscard]] std::vector<int> LoadedPlayerSceneHandles(
        const LocalEntityHandle localPlayer) noexcept {
        try {
            std::scoped_lock lock{sync_};
            std::vector<int> handles;
            if (localPlayer == 0) {
                return handles;
            }
            for (const auto& [handle, scene] : scenes_) {
                if (!scene.loadRequested || scene.complete || handle <= 0 ||
                    std::none_of(
                        scene.roles.begin(),
                        scene.roles.end(),
                        [localPlayer](const auto& role) {
                            return role.localHandle == localPlayer;
                        })) {
                    continue;
                }
                handles.push_back(handle);
            }
            return handles;
        } catch (...) {
            return {};
        }
    }

    [[nodiscard]] std::vector<LocalEntityHandle>
    ActiveRoleEntityHandles() noexcept {
        try {
            std::scoped_lock lock{sync_};
            std::vector<LocalEntityHandle> handles;
            for (const auto& [sceneHandle, scene] : scenes_) {
                (void)sceneHandle;
                if (!scene.complete) {
                    continue;
                }
                for (const auto& role : scene.roles) {
                    if (role.localHandle == 0 ||
                        std::find(
                            handles.begin(),
                            handles.end(),
                            role.localHandle) != handles.end()) {
                        continue;
                    }
                    handles.push_back(role.localHandle);
                }
            }
            return handles;
        } catch (...) {
            return {};
        }
    }

    void Clear() noexcept {
        try {
            std::scoped_lock lock{sync_};
            scenes_.clear();
            completed_.clear();
        } catch (...) {
        }
    }

private:
    [[nodiscard]] std::uint64_t NextSequence() noexcept {
        ++nextSequence_;
        if (nextSequence_ == 0U) {
            ++nextSequence_;
        }
        return nextSequence_;
    }

    std::mutex sync_{};
    std::unordered_map<int, RawCapturedAnimScene> scenes_{};
    std::deque<RawCapturedAnimScene> completed_{};
    std::uint64_t nextSequence_{};
};

StoryVmAnimSceneCapture g_storyVmAnimSceneCapture{};
std::array<AnimSceneInlineHook,
           static_cast<std::size_t>(AnimSceneCaptureHookKind::Count)>
    g_animSceneCaptureHooks{};
bool g_animSceneCaptureHooksInstalled{};

[[nodiscard]] bool CopyNativeAsciiBytes(
    const char* source,
    char* destination,
    const std::size_t maximumBytes,
    std::size_t* copiedBytes) noexcept {
    if (source == nullptr || destination == nullptr || copiedBytes == nullptr ||
        maximumBytes == 0U) {
        return false;
    }
    __try {
        for (std::size_t index = 0U; index <= maximumBytes; ++index) {
            const auto value = static_cast<unsigned char>(source[index]);
            if (value == 0U) {
                destination[index] = '\0';
                *copiedBytes = index;
                return index != 0U;
            }
            if (index == maximumBytes || value < 0x20U || value > 0x7EU) {
                return false;
            }
            destination[index] = static_cast<char>(value);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return false;
}

[[nodiscard]] std::optional<std::string> CopyNativeAsciiString(
    const std::uint64_t pointerValue,
    const std::size_t maximumBytes) {
    if (pointerValue == 0U || maximumBytes > kMaximumAnimSceneResourceBytes) {
        return std::nullopt;
    }
    std::array<char, kMaximumAnimSceneResourceBytes + 1U> buffer{};
    std::size_t copied{};
    if (!CopyNativeAsciiBytes(
            reinterpret_cast<const char*>(pointerValue),
            buffer.data(),
            maximumBytes,
            &copied)) {
        return std::nullopt;
    }
    return std::string{buffer.data(), copied};
}

[[nodiscard]] std::uint64_t NativeArgument(
    const NativeHandlerContextView* context,
    const std::size_t index) noexcept {
    if (context == nullptr || context->arguments == nullptr ||
        index >= context->argumentCount) {
        return 0U;
    }
    return context->arguments[index];
}

void CallCapturedOriginal(
    const AnimSceneCaptureHookKind kind,
    NativeHandlerContextView* context) noexcept {
    const auto original = g_animSceneCaptureHooks[
        static_cast<std::size_t>(kind)].trampoline;
    if (original != nullptr) {
        original(context);
    }
}

void __fastcall CaptureCreateAnimScene(
    NativeHandlerContextView* context) noexcept {
    bool originalCalled{};
    try {
        const auto resource = CopyNativeAsciiString(
            NativeArgument(context, 0U),
            kMaximumAnimSceneResourceBytes);
        const auto playback = CopyNativeAsciiString(
            NativeArgument(context, 2U),
            kMaximumAnimScenePlaybackListBytes);
        const auto sceneFlags = static_cast<std::uint32_t>(
            NativeArgument(context, 1U));
        std::uint8_t options{};
        if (NativeArgument(context, 3U) != 0U) {
            options |= 1U;
        }
        if (NativeArgument(context, 4U) != 0U) {
            options |= 2U;
        }
        originalCalled = true;
        CallCapturedOriginal(AnimSceneCaptureHookKind::Create, context);
        if (resource.has_value() && context != nullptr &&
            context->returnValues != nullptr) {
            const auto scene = static_cast<int>(*context->returnValues);
            g_storyVmAnimSceneCapture.OnCreate(
                scene,
                sceneFlags,
                options,
                *resource,
                playback.value_or(std::string{}));
        }
        return;
    } catch (...) {
    }
    if (!originalCalled) {
        CallCapturedOriginal(AnimSceneCaptureHookKind::Create, context);
    }
}

void __fastcall CaptureSetAnimSceneEntity(
    NativeHandlerContextView* context) noexcept {
    bool originalCalled{};
    try {
        const auto scene = static_cast<int>(NativeArgument(context, 0U));
        const auto role = CopyNativeAsciiString(
            NativeArgument(context, 1U),
            kMaximumAnimSceneRoleNameBytes);
        const auto entity = static_cast<LocalEntityHandle>(
            NativeArgument(context, 2U));
        const auto flags = static_cast<std::uint32_t>(
            NativeArgument(context, 3U));
        originalCalled = true;
        CallCapturedOriginal(AnimSceneCaptureHookKind::SetEntity, context);
        if (role.has_value()) {
            g_storyVmAnimSceneCapture.OnSetEntity(
                scene,
                *role,
                entity,
                flags);
        }
        return;
    } catch (...) {
    }
    if (!originalCalled) {
        CallCapturedOriginal(AnimSceneCaptureHookKind::SetEntity, context);
    }
}

void __fastcall CaptureRemoveAnimSceneEntity(
    NativeHandlerContextView* context) noexcept {
    bool originalCalled{};
    try {
        const auto scene = static_cast<int>(NativeArgument(context, 0U));
        const auto role = CopyNativeAsciiString(
            NativeArgument(context, 1U),
            kMaximumAnimSceneRoleNameBytes);
        originalCalled = true;
        CallCapturedOriginal(AnimSceneCaptureHookKind::RemoveEntity, context);
        if (role.has_value()) {
            g_storyVmAnimSceneCapture.OnRemoveEntity(scene, *role);
        }
        return;
    } catch (...) {
    }
    if (!originalCalled) {
        CallCapturedOriginal(AnimSceneCaptureHookKind::RemoveEntity, context);
    }
}

void __fastcall CaptureSetAnimScenePlaybackList(
    NativeHandlerContextView* context) noexcept {
    bool originalCalled{};
    try {
        const auto scene = static_cast<int>(NativeArgument(context, 0U));
        const auto playback = CopyNativeAsciiString(
            NativeArgument(context, 1U),
            kMaximumAnimScenePlaybackListBytes);
        originalCalled = true;
        CallCapturedOriginal(AnimSceneCaptureHookKind::PlaybackList, context);
        if (playback.has_value()) {
            g_storyVmAnimSceneCapture.OnPlaybackList(scene, *playback);
        }
        return;
    } catch (...) {
    }
    if (!originalCalled) {
        CallCapturedOriginal(AnimSceneCaptureHookKind::PlaybackList, context);
    }
}

void __fastcall CaptureLoadAnimScene(
    NativeHandlerContextView* context) noexcept {
    const auto scene = static_cast<int>(NativeArgument(context, 0U));
    CallCapturedOriginal(AnimSceneCaptureHookKind::Load, context);
    try {
        g_storyVmAnimSceneCapture.OnLoad(scene);
    } catch (...) {
    }
}

void __fastcall CaptureStartAnimScene(
    NativeHandlerContextView* context) noexcept {
    const auto scene = static_cast<int>(NativeArgument(context, 0U));
    CallCapturedOriginal(AnimSceneCaptureHookKind::Start, context);
    try {
        g_storyVmAnimSceneCapture.OnStart(scene);
    } catch (...) {
    }
}

void __fastcall CaptureDeleteAnimScene(
    NativeHandlerContextView* context) noexcept {
    const auto scene = static_cast<int>(NativeArgument(context, 0U));
    CallCapturedOriginal(AnimSceneCaptureHookKind::Delete, context);
    try {
        g_storyVmAnimSceneCapture.OnDelete(scene);
    } catch (...) {
    }
}

void WriteAbsoluteJump(
    std::uint8_t* destination,
    const void* target) noexcept {
    destination[0] = 0x48U;
    destination[1] = 0xB8U;
    const auto address = reinterpret_cast<std::uintptr_t>(target);
    std::memcpy(destination + 2U, &address, sizeof(address));
    destination[10] = 0xFFU;
    destination[11] = 0xE0U;
}

// Trampolines must not destroy any register produced by the copied native
// handler prefix. In particular, SET_ANIM_SCENE_ENTITY leaves the argument
// array in RAX and consumes it immediately after the 12-byte prefix. The old
// `mov rax, target; jmp rax` continuation replaced that live value with the
// continuation address, so the original game handler received corrupt role
// and entity arguments. An RIP-relative indirect jump is two bytes longer but
// preserves every general-purpose register.
void WriteRegisterPreservingAbsoluteJump(
    std::uint8_t* destination,
    const void* target) noexcept {
    destination[0] = 0xFFU;
    destination[1] = 0x25U;
    destination[2] = 0x00U;
    destination[3] = 0x00U;
    destination[4] = 0x00U;
    destination[5] = 0x00U;
    const auto address = reinterpret_cast<std::uintptr_t>(target);
    std::memcpy(destination + 6U, &address, sizeof(address));
}

[[nodiscard]] bool InstallAnimSceneInlineHook(
    AnimSceneInlineHook& hook,
    std::uint8_t* target,
    const NativeHandlerFunction replacement,
    const std::span<const std::uint8_t> expected,
    const std::size_t copiedPrefixLength,
    const std::optional<std::size_t> terminalRelativeJumpOffset,
    std::string& reason) {
    reason.clear();
    const auto overwriteLength = std::max<std::size_t>(12U, expected.size());
    if (target == nullptr || replacement == nullptr ||
        overwriteLength > hook.original.size() ||
        copiedPrefixLength > expected.size() ||
        std::memcmp(target, expected.data(), expected.size()) != 0) {
        reason = "handler-prologue-mismatch";
        return false;
    }
    auto* trampoline = static_cast<std::uint8_t*>(::VirtualAlloc(
        nullptr,
        64U,
        MEM_COMMIT | MEM_RESERVE,
        PAGE_READWRITE));
    if (trampoline == nullptr) {
        reason = "trampoline-allocation-failed";
        return false;
    }
    std::memcpy(trampoline, target, copiedPrefixLength);
    const void* continuation = target + copiedPrefixLength;
    if (terminalRelativeJumpOffset.has_value()) {
        std::int32_t displacement{};
        std::memcpy(
            &displacement,
            target + *terminalRelativeJumpOffset + 1U,
            sizeof(displacement));
        continuation =
            target + *terminalRelativeJumpOffset + 5U + displacement;
    }
    WriteRegisterPreservingAbsoluteJump(
        trampoline + copiedPrefixLength,
        continuation);
    DWORD trampolineProtection{};
    if (::VirtualProtect(
            trampoline,
            64U,
            PAGE_EXECUTE_READ,
            &trampolineProtection) == FALSE) {
        ::VirtualFree(trampoline, 0U, MEM_RELEASE);
        reason = "trampoline-protect-failed";
        return false;
    }
    ::FlushInstructionCache(
        ::GetCurrentProcess(),
        trampoline,
        copiedPrefixLength + 14U);

    DWORD oldProtection{};
    if (::VirtualProtect(
            target,
            overwriteLength,
            PAGE_EXECUTE_READWRITE,
            &oldProtection) == FALSE) {
        ::VirtualFree(trampoline, 0U, MEM_RELEASE);
        reason = "handler-protect-failed";
        return false;
    }
    std::memcpy(hook.original.data(), target, overwriteLength);
    WriteAbsoluteJump(target, reinterpret_cast<const void*>(replacement));
    std::fill(target + 12U, target + overwriteLength, 0x90U);
    ::FlushInstructionCache(
        ::GetCurrentProcess(),
        target,
        overwriteLength);
    DWORD ignoredProtection{};
    (void)::VirtualProtect(
        target,
        overwriteLength,
        oldProtection,
        &ignoredProtection);

    hook.target = target;
    hook.replacement = replacement;
    hook.trampoline = reinterpret_cast<NativeHandlerFunction>(trampoline);
    hook.overwriteLength = overwriteLength;
    hook.trampolineMemory = trampoline;
    hook.installed = true;
    return true;
}

void RemoveAnimSceneCaptureHooks() noexcept {
    for (auto iterator = g_animSceneCaptureHooks.rbegin();
         iterator != g_animSceneCaptureHooks.rend();
         ++iterator) {
        auto& hook = *iterator;
        if (!hook.installed || hook.target == nullptr) {
            continue;
        }
        DWORD oldProtection{};
        if (::VirtualProtect(
                hook.target,
                hook.overwriteLength,
                PAGE_EXECUTE_READWRITE,
                &oldProtection) != FALSE) {
            std::memcpy(
                hook.target,
                hook.original.data(),
                hook.overwriteLength);
            ::FlushInstructionCache(
                ::GetCurrentProcess(),
                hook.target,
                hook.overwriteLength);
            DWORD ignoredProtection{};
            (void)::VirtualProtect(
                hook.target,
                hook.overwriteLength,
                oldProtection,
                &ignoredProtection);
        }
        if (hook.trampolineMemory != nullptr) {
            ::VirtualFree(hook.trampolineMemory, 0U, MEM_RELEASE);
        }
        hook = {};
    }
    g_animSceneCaptureHooksInstalled = false;
    g_storyVmAnimSceneCapture.Clear();
}

[[nodiscard]] bool InstallAnimSceneCaptureHooks(
    const std::array<std::uint8_t*, 7U>& handlers,
    std::string& reason) {
    if (g_animSceneCaptureHooksInstalled) {
        return true;
    }
    static constexpr std::array<std::uint8_t, 13U> kCreateExpected{
        0x40U, 0x53U, 0x48U, 0x83U, 0xECU, 0x30U, 0x4CU,
        0x8BU, 0x49U, 0x10U, 0x48U, 0x8BU, 0xD9U};
    static constexpr std::array<std::uint8_t, 12U> kSetExpected{
        0x48U, 0x8BU, 0x41U, 0x10U, 0x44U, 0x8BU,
        0x48U, 0x18U, 0x44U, 0x8BU, 0x40U, 0x10U};
    static constexpr std::array<std::uint8_t, 12U> kRemoveExpected{
        0x48U, 0x8BU, 0x41U, 0x10U, 0x44U, 0x8BU,
        0x40U, 0x10U, 0x48U, 0x8BU, 0x50U, 0x08U};
    static constexpr std::array<std::uint8_t, 15U> kPlaybackExpected{
        0x48U, 0x8BU, 0x41U, 0x10U, 0x48U, 0x8BU, 0x50U, 0x08U,
        0x8BU, 0x08U, 0xE9U, 0x91U, 0x08U, 0x01U, 0x00U};
    static constexpr std::array<std::uint8_t, 11U> kLoadExpected{
        0x48U, 0x8BU, 0x41U, 0x10U, 0x8BU, 0x08U,
        0xE9U, 0x71U, 0x96U, 0x00U, 0x00U};
    static constexpr std::array<std::uint8_t, 11U> kStartExpected{
        0x48U, 0x8BU, 0x41U, 0x10U, 0x8BU, 0x08U,
        0xE9U, 0xA5U, 0x28U, 0x01U, 0x00U};
    static constexpr std::array<std::uint8_t, 11U> kDeleteExpected{
        0x48U, 0x8BU, 0x41U, 0x10U, 0x8BU, 0x08U,
        0xE9U, 0x55U, 0xA8U, 0x00U, 0x00U};
    const std::array<NativeHandlerFunction, 7U> replacements{
        CaptureCreateAnimScene,
        CaptureSetAnimSceneEntity,
        CaptureRemoveAnimSceneEntity,
        CaptureSetAnimScenePlaybackList,
        CaptureLoadAnimScene,
        CaptureStartAnimScene,
        CaptureDeleteAnimScene};
    const std::array<std::span<const std::uint8_t>, 7U> expected{
        kCreateExpected,
        kSetExpected,
        kRemoveExpected,
        kPlaybackExpected,
        kLoadExpected,
        kStartExpected,
        kDeleteExpected};
    const std::array<std::size_t, 7U> copiedPrefixLengths{
        13U, 12U, 12U, 10U, 6U, 6U, 6U};
    const std::array<std::optional<std::size_t>, 7U> tailJumps{
        std::nullopt,
        std::nullopt,
        std::nullopt,
        10U,
        6U,
        6U,
        6U};

    for (std::size_t index = 0U; index < handlers.size(); ++index) {
        if (!InstallAnimSceneInlineHook(
                g_animSceneCaptureHooks[index],
                handlers[index],
                replacements[index],
                expected[index],
                copiedPrefixLengths[index],
                tailJumps[index],
                reason)) {
            RemoveAnimSceneCaptureHooks();
            return false;
        }
    }
    g_animSceneCaptureHooksInstalled = true;
    return true;
}
#endif
// Context variants used by Story interaction prompts. They are disabled only
// while the reversible guest mask has identified a nearby local mission actor;
// movement, combat and interaction with replicated host entities remain live.
constexpr std::array<int, 5> kGuestStoryContextControls{
    static_cast<int>(0xCEFD9220U),
    static_cast<int>(0xC1989F95U),
    static_cast<int>(0x760A9C6FU),
    static_cast<int>(0x5181713DU),
    static_cast<int>(0x3B24C470U)};
constexpr std::uint64_t kLocalTraversalProbeIntervalMs = 40U;
constexpr std::uint64_t kLocalTraversalProbeFreshnessMs = 250U;
constexpr float kLocalTraversalProbeLengthMeters = 1.75F;
constexpr float kLocalTraversalProbeRadiusMeters = 0.28F;
constexpr int kTraversalShapeTestFlags = 1 | 16;
constexpr std::uint64_t kRemoteTraversalGeometryProbeTimeoutMs = 350U;
constexpr float kRemoteTraversalGeometryToleranceMeters = 2.75F;
constexpr float kAnimGraphReplicaCoordinateDeadZoneMeters = 0.015F;
constexpr float kAnimGraphReplicaHeadingDeadZoneDegrees = 0.15F;
constexpr int kAnimGraphVisualTaskTimeoutMilliseconds = 10'000;
constexpr int kAnimGraphVisualIdleTaskMilliseconds = 3'500;
constexpr int kAnimGraphTurnInPlaceTimeoutMilliseconds = 750;
constexpr float kAnimGraphVisualTaskStoppingRangeMeters = 0.05F;

[[nodiscard]] constexpr std::uint32_t RageJoaat(
    const std::string_view value) noexcept {
    std::uint32_t hash{};
    for (const auto raw : value) {
        auto character = static_cast<std::uint8_t>(raw);
        if (character >= static_cast<std::uint8_t>('A') &&
            character <= static_cast<std::uint8_t>('Z')) {
            character = static_cast<std::uint8_t>(
                character +
                (static_cast<std::uint8_t>('a') -
                 static_cast<std::uint8_t>('A')));
        }
        hash += character;
        hash += hash << 10U;
        hash ^= hash >> 6U;
    }
    hash += hash << 3U;
    hash ^= hash >> 11U;
    hash += hash << 15U;
    return hash;
}

inline constexpr std::uint32_t kMotionStateIdle =
    RageJoaat("motionstate_idle");
inline constexpr std::uint32_t kMotionStateWalk =
    RageJoaat("motionstate_walk");
inline constexpr std::uint32_t kMotionStateRun =
    RageJoaat("motionstate_run");
inline constexpr std::uint32_t kMotionStateSprint =
    RageJoaat("motionstate_sprint");
inline constexpr std::uint32_t kWeaponLasso = RageJoaat("weapon_lasso");
inline constexpr std::uint32_t kWeaponUnarmed = RageJoaat("weapon_unarmed");
inline constexpr std::uint32_t kFiringPatternSingleShot =
    RageJoaat("firing_pattern_single_shot");
constexpr int kRemoteVisualFireTaskMilliseconds = 120;
constexpr std::uint64_t kRemoteVisualFireAmmoRestoreMilliseconds = 170U;

#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
[[nodiscard]] BOOL CanPedBeMounted(const Ped ped) {
    return invoke<BOOL>(0x2D64376CF437363E, ped);
}

[[nodiscard]] bool IsPedHumanReliable(
    const Ped ped,
    int& pedType,
    bool& usedPedTypeFallback) {
    pedType = PED::GET_PED_TYPE(ped);
    const bool nativeHuman =
        PED::IS_PED_HUMAN(ped) != FALSE;
    // On the pinned Story Mode build IS_PED_HUMAN has been observed returning
    // false for an entire valid pool of town residents. GET_PED_TYPE is the
    // independent SDK-native discriminator: 4=male, 5=female, 28=animal.
    // Keep IS_PED_HUMAN as the primary signal and use only the documented
    // human values as a conservative fallback; wildlife is never promoted.
    const bool pedTypeHuman = pedType == 4 || pedType == 5;
    usedPedTypeFallback = !nativeHuman && pedTypeHuman;
    return nativeHuman || pedTypeHuman;
}

[[nodiscard]] Player GetPlayerOwnerOfMount(const Ped mount) {
    return invoke<Player>(0xAD03B03737CE6810, mount);
}

[[nodiscard]] BOOL IsPedLassoed(const Ped ped) {
    return invoke<BOOL>(0x9682F850056C9ADE, ped);
}

[[nodiscard]] BOOL IsPedBeingHogtied(const Ped ped) {
    return invoke<BOOL>(0xD453BB601D4A606E, ped);
}

[[nodiscard]] BOOL IsPedHogtied(const Ped ped) {
    return invoke<BOOL>(0x3AA24CCC0D451379, ped);
}

// RDR2's crouch locomotion is separate from the GTA-style stealth flag
// exposed by the old SDK header. The sender and receiver both use the game's
// native crouch graph so walking/sprinting while crouched remains visible.
[[nodiscard]] BOOL GetPedCrouchMovement(const Ped ped) {
    return invoke<BOOL>(0xD5FE956C70FF370B, ped);
}

void SetPedCrouchMovement(
    const Ped ped,
    const BOOL enabled,
    const BOOL immediately) {
    invoke<Void>(
        0x7DE9692C6F64CFE8,
        ped,
        enabled,
        0,
        immediately);
}

[[nodiscard]] Ped GetLassoTarget(const Ped lassoer) {
    return invoke<Ped>(0xB65A4DAB460A19BD, lassoer);
}

void TaskLassoPed(const Ped lassoer, const Ped target) {
    invoke<Void>(0xC716EB2BD16370A3, lassoer, target);
}

[[nodiscard]] BOOL GetPedLassoHogtieFlag(
    const Ped ped,
    const int flag) {
    return invoke<BOOL>(0x2C76FA0E01681F8D, ped, flag);
}

void SetPedLassoHogtieFlag(
    const Ped ped,
    const int flag,
    const BOOL enabled) {
    invoke<Void>(0xAE6004120C18DF97, ped, flag, enabled);
}

void SetPedCanBeLassoed(const Ped ped, const BOOL enabled) {
    invoke<Void>(0xFD6943B6DF77E449, ped, enabled);
}

void SetPedOntoMount(
    const Ped rider,
    const Ped mount) {
    invoke<Void>(
        0x028F76B6E78246EB,
        rider,
        mount,
        -1,
        TRUE);
}

void TaskDismountAnimal(const Ped rider) {
    invoke<Void>(
        0x48E92D3DDE23C23A,
        rider,
        0,
        0,
        0,
        0,
        0);
}

// RDR2 does not initialize MetaPed components for every CREATE_PED result.
// Without this native the entity can collide and leave footprints while its
// body remains completely invisible.
void SetRandomOutfitVariation(const Ped ped) {
    invoke<Void>(0x283978A15512B2FE, ped, FALSE);
}

[[nodiscard]] Hash GetShopItemComponentAtIndex(
    const Ped ped,
    const int index) {
    // Both output records are intentionally over-allocated. Their layout is
    // private to MetaPed and is not placed on the wire; the native only needs
    // valid writable storage while returning the portable shop-item hash.
    std::array<std::uint64_t, 16> componentData{};
    std::array<std::uint64_t, 16> variationData{};
    return invoke<Hash>(
        0x77BA37622E22023B,
        ped,
        index,
        FALSE,
        componentData.data(),
        variationData.data());
}

void ResetPedComponents(const Ped ped) {
    invoke<Void>(0x0BFA1BD465CDFEFD, ped);
}

void ApplyShopItemToPed(
    const Ped ped,
    const Hash componentHash) {
    invoke<Void>(
        0xD3A7B003ED343FD9,
        ped,
        componentHash,
        TRUE,
        FALSE,
        FALSE);
}

void UpdatePedVariation(const Ped ped) {
    invoke<Void>(
        0xCC8CA3E88256E58F,
        ped,
        FALSE,
        TRUE,
        TRUE,
        TRUE,
        FALSE);
}

[[nodiscard]] BOOL DoesAnimSceneExist(const int scene) {
    return invoke<BOOL>(0x25557E324489393C, scene);
}

[[nodiscard]] BOOL IsAnimSceneLoaded(const int scene) {
    return invoke<BOOL>(0x477122B8D05E7968, scene, TRUE, FALSE);
}

[[nodiscard]] BOOL IsAnimSceneRunning(const int scene) {
    return invoke<BOOL>(0xCBFC7725DE6CE2E0, scene, FALSE);
}

[[nodiscard]] Hash GetAnimSceneDictionary(const int scene) {
    return invoke<Hash>(0xAE5ADA4FE3E21ADC, scene);
}

[[nodiscard]] float GetAnimSceneDuration(const int scene) {
    return invoke<float>(0x49F1D143ADE32656, scene);
}

[[nodiscard]] float GetAnimSceneRate(const int scene) {
    return invoke<float>(0x43C21623E42B821B, scene);
}

[[nodiscard]] float GetAnimScenePhase(const int scene) {
    return invoke<float>(0x3FBC3F51BF12DFBF, scene);
}

[[nodiscard]] int GetAnimSceneActiveCameraCount(const int scene) {
    return invoke<int>(0x4822A65D5AF64E69, scene);
}

void GetAnimSceneOrigin(
    const int scene,
    Vector3& position,
    Vector3& rotation) {
    invoke<Void>(
        0xADF1D53F3B1FE0A7,
        scene,
        &position,
        &rotation,
        2);
}

void SetAnimSceneOrigin(
    const int scene,
    const Vec3& position,
    const Vec3& rotation) {
    invoke<Void>(
        0x020894BF17A02EF2,
        scene,
        position.x,
        position.y,
        position.z,
        rotation.x,
        rotation.y,
        rotation.z,
        2);
}

void SetAnimScenePaused(const int scene, const bool paused) {
    invoke<Void>(
        0xD6824B7D24DC0CE0,
        scene,
        paused ? TRUE : FALSE);
}

void SetAnimSceneRate(const int scene, const float rate) {
    invoke<Void>(0x75820B801CFF262A, scene, rate);
}

void StartAnimScene(const int scene) {
    invoke<Void>(0xF4D94AF761768700, scene);
}

[[nodiscard]] int CreateAnimScene(
    const char* resourceName,
    const std::uint32_t sceneFlags,
    const char* playbackList,
    const std::uint8_t optionFlags) {
    return invoke<int>(
        0x1FCA98E33C1437B3,
        resourceName,
        static_cast<int>(sceneFlags),
        playbackList,
        (optionFlags & 1U) != 0U ? TRUE : FALSE,
        (optionFlags & 2U) != 0U ? TRUE : FALSE);
}

void SetAnimSceneEntity(
    const int scene,
    const char* roleName,
    const Entity entity,
    const std::uint32_t bindingFlags) {
    invoke<Void>(
        0x8B720AD451CA2AB3,
        scene,
        roleName,
        entity,
        static_cast<int>(bindingFlags));
}

void SetAnimScenePlaybackList(
    const int scene,
    const char* playbackList) {
    invoke<Void>(0xAB5E7CAB074D6B84, scene, playbackList);
}

void LoadAnimScene(const int scene) {
    invoke<Void>(0x8A8208AE92BF87A5, scene);
}

void DeleteAnimScene(const int scene) {
    invoke<Void>(0x84EEDB2C6E650000, scene);
}
#endif

[[nodiscard]] std::string_view RemoteLocomotionName(
    const RemoteLocomotion locomotion) noexcept {
    switch (locomotion) {
        case RemoteLocomotion::Idle:
            return "idle";
        case RemoteLocomotion::Walk:
            return "walk";
        case RemoteLocomotion::Run:
            return "run";
        case RemoteLocomotion::Sprint:
            return "sprint";
    }
    return "unknown";
}

[[nodiscard]] std::string_view PuppetControlModeName(
    const PuppetControlMode mode) noexcept {
    switch (mode) {
        case PuppetControlMode::GroundedLocomotion:
            return "grounded";
        case PuppetControlMode::AimingLocomotion:
            return "aiming";
        case PuppetControlMode::TraversalApproach:
            return "traversal-approach";
        case PuppetControlMode::TraversalCommitted:
            return "traversal-committed";
        case PuppetControlMode::Airborne:
            return "airborne";
        case PuppetControlMode::RagdollOrLasso:
            return "ragdoll-lasso";
        case PuppetControlMode::Mounted:
            return "mounted";
        case PuppetControlMode::NavRecovery:
            return "nav-recovery";
        case PuppetControlMode::HardResync:
            return "hard-resync";
    }
    return "unknown";
}

[[nodiscard]] bool IsMirrorablePopulationType(
    const int populationType) noexcept {
    // Pool observations across Story Mode builds include ordinary ambient
    // residents under type 0 and script/mission peds under type 7. The host
    // still filters players, local/remote mounts and every proxy registry, so
    // accepting the complete native 0-7 ped range is safer than producing an
    // empty authoritative world.
    return populationType >= 0 && populationType <= 7;
}

[[nodiscard]] float AbsoluteHeadingDifference(
    float from,
    float to) noexcept {
    if (!std::isfinite(from) || !std::isfinite(to)) {
        return 0.0F;
    }
    from = std::fmod(from, 360.0F);
    to = std::fmod(to, 360.0F);
    auto difference = std::abs(to - from);
    if (difference > 180.0F) {
        difference = 360.0F - difference;
    }
    return difference;
}

[[nodiscard]] float NormalizeHeading(float heading) noexcept {
    if (!std::isfinite(heading)) {
        return 0.0F;
    }
    heading = std::fmod(heading, 360.0F);
    return heading < 0.0F ? heading + 360.0F : heading;
}

[[nodiscard]] std::optional<std::filesystem::path>
BridgeLogPath() {
    std::array<wchar_t, 32'768> localAppData{};
    const auto length = ::GetEnvironmentVariableW(
        L"LOCALAPPDATA",
        localAppData.data(),
        static_cast<DWORD>(localAppData.size()));
    if (length == 0U || length >= localAppData.size()) {
        return std::nullopt;
    }

    return std::filesystem::path{
               std::wstring_view{localAppData.data(), length}} /
        L"RDR2CoopStory" /
        L"launcher" /
        L"logs" /
        L"bridge.log";
}

void AppendPersistentBridgeLog(
    const std::string_view message) {
    const auto path = BridgeLogPath();
    if (!path.has_value()) {
        return;
    }

    std::error_code error;
    std::filesystem::create_directories(
        path->parent_path(),
        error);
    if (error) {
        return;
    }

    const auto currentSize =
        std::filesystem::file_size(*path, error);
    if (!error && currentSize > kBridgeLogMaxBytes) {
        const auto archivePath = [&path](
                                     const unsigned int index) {
            auto result = *path;
            result += "." + std::to_string(index);
            return result;
        };
        std::filesystem::remove(
            archivePath(kBridgeLogArchiveCount),
            error);
        error.clear();
        for (auto index = kBridgeLogArchiveCount;
             index > 1U;
             --index) {
            const auto previous = archivePath(index - 1U);
            if (!std::filesystem::exists(previous, error)) {
                error.clear();
                continue;
            }
            std::filesystem::rename(
                previous,
                archivePath(index),
                error);
            if (error) {
                return;
            }
        }
        std::filesystem::rename(
            *path,
            archivePath(1U),
            error);
        if (error) {
            return;
        }
    }

    SYSTEMTIME utc{};
    ::GetSystemTime(&utc);
    std::array<char, 64> timestamp{};
    const auto timestampLength = std::snprintf(
        timestamp.data(),
        timestamp.size(),
        "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
        utc.wYear,
        utc.wMonth,
        utc.wDay,
        utc.wHour,
        utc.wMinute,
        utc.wSecond,
        utc.wMilliseconds);
    if (timestampLength <= 0 ||
        static_cast<std::size_t>(timestampLength) >=
            timestamp.size()) {
        return;
    }

    std::ofstream output{
        *path,
        std::ios::binary | std::ios::app};
    if (!output) {
        return;
    }
    output.write(timestamp.data(), timestampLength);
    output << " [CoopStoryBridge] ";
    output.write(
        message.data(),
        static_cast<std::streamsize>(message.size()));
    output.put('\n');
    output.flush();
}

[[nodiscard]] bool Pressed(const int virtualKey) noexcept {
    return (::GetAsyncKeyState(virtualKey) & 0x8000) != 0;
}

[[nodiscard]] std::string Utf8FromWide(
    const std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const auto length = ::WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (length <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(length), '\0');
    if (::WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            length,
            nullptr,
            nullptr) != length) {
        return {};
    }
    return result;
}

[[nodiscard]] std::wstring WideFromUtf8(
    const std::string_view value) {
    if (value.empty()) {
        return {};
    }
    const auto length = ::MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (length <= 0) {
        return {};
    }
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (::MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            length) != length) {
        return {};
    }
    return result;
}

#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS

// UNVERIFIED_NATIVE_BINDING:
// The wrappers below are public SDK natives, but the SDK predates the pinned
// 1491.50 executable. Runtime behavior must be verified on that exact build
// before this opt-in is enabled. The default build compiles this block out.
[[nodiscard]] bool IsExecutableProtection(
    const DWORD protection) noexcept {
    if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0U) {
        return false;
    }
    switch (protection & 0xFFU) {
        case PAGE_EXECUTE:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] std::string HexValue(
    const std::uint64_t value,
    const unsigned int minimumDigits = 1U) {
    std::array<char, 32> buffer{};
    const auto written = std::snprintf(
        buffer.data(),
        buffer.size(),
        "0x%0*llX",
        static_cast<int>(minimumDigits),
        static_cast<unsigned long long>(value));
    return written > 0 &&
                   static_cast<std::size_t>(written) < buffer.size()
               ? std::string{buffer.data(),
                             static_cast<std::size_t>(written)}
               : std::string{"0x0"};
}

[[nodiscard]] std::string HexBytes(
    const std::uint8_t* bytes,
    const std::size_t size) {
    if (bytes == nullptr || size == 0U) {
        return "none";
    }
    constexpr char kHex[] = "0123456789ABCDEF";
    std::string result;
    result.reserve(size * 2U);
    for (std::size_t index = 0U; index < size; ++index) {
        const auto value = bytes[index];
        result.push_back(kHex[value >> 4U]);
        result.push_back(kHex[value & 0x0FU]);
    }
    return result;
}

struct PinnedScriptHookModule final {
    HMODULE module{};
    const std::uint8_t* base{};
};

[[nodiscard]] std::optional<PinnedScriptHookModule>
FindPinnedScriptHookModule(std::string& reason) {
    const auto module = ::GetModuleHandleW(L"ScriptHookRDR2.dll");
    if (module == nullptr) {
        reason = "module-not-loaded";
        return std::nullopt;
    }
    const auto* base = reinterpret_cast<const std::uint8_t*>(module);
    MEMORY_BASIC_INFORMATION moduleMemory{};
    if (::VirtualQuery(base, &moduleMemory, sizeof(moduleMemory)) == 0U ||
        moduleMemory.State != MEM_COMMIT ||
        moduleMemory.AllocationBase != module) {
        reason = "module-memory-invalid";
        return std::nullopt;
    }

    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        dos->e_lfanew > 0x1000) {
        reason = "dos-header-mismatch";
        return std::nullopt;
    }
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(
        base + static_cast<std::size_t>(dos->e_lfanew));
    if (nt->Signature != IMAGE_NT_SIGNATURE ||
        nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->FileHeader.TimeDateStamp != kPinnedScriptHookPeTimestamp ||
        nt->OptionalHeader.SizeOfImage != kPinnedScriptHookImageSize) {
        reason = "pe-identity-mismatch";
        return std::nullopt;
    }

    const auto nativeInitAddress = reinterpret_cast<const std::uint8_t*>(
        ::GetProcAddress(module, "?nativeInit@@YAX_K@Z"));
    const auto nativeCallAddress = reinterpret_cast<const std::uint8_t*>(
        ::GetProcAddress(module, "?nativeCall@@YAPEA_KXZ"));
    const auto handleBaseAddress = reinterpret_cast<const std::uint8_t*>(
        ::GetProcAddress(
            module,
            "?getScriptHandleBaseAddress@@YAPEAEH@Z"));
    // SDK import libraries have used both decorated and undecorated exports
    // across releases. Fall back to the public names before rejecting.
    const auto resolvedNativeInit = nativeInitAddress != nullptr
                                        ? nativeInitAddress
                                        : reinterpret_cast<const std::uint8_t*>(
                                              ::GetProcAddress(
                                                  module,
                                                  "nativeInit"));
    const auto resolvedNativeCall = nativeCallAddress != nullptr
                                        ? nativeCallAddress
                                        : reinterpret_cast<const std::uint8_t*>(
                                              ::GetProcAddress(
                                                  module,
                                                  "nativeCall"));
    const auto resolvedHandleBase = handleBaseAddress != nullptr
                                        ? handleBaseAddress
                                        : reinterpret_cast<const std::uint8_t*>(
                                              ::GetProcAddress(
                                                  module,
                                                  "getScriptHandleBaseAddress"));
    constexpr std::array<std::uint8_t, 5> kNativeInitStub{
        0xE9U, 0xFBU, 0x0EU, 0x00U, 0x00U};
    constexpr std::array<std::uint8_t, 5> kNativeCallStub{
        0xE9U, 0x0BU, 0x10U, 0x00U, 0x00U};
    constexpr std::array<std::uint8_t, 8> kHandleBaseStub{
        0x44U, 0x8BU, 0xC1U, 0x83U,
        0xF9U, 0xFFU, 0x74U, 0x42U};
    if (resolvedNativeInit != base + kPinnedScriptHookNativeInitRva ||
        resolvedNativeCall != base + kPinnedScriptHookNativeCallRva ||
        resolvedHandleBase != base + kPinnedScriptHookHandleBaseRva ||
        std::memcmp(
            resolvedNativeInit,
            kNativeInitStub.data(),
            kNativeInitStub.size()) != 0 ||
        std::memcmp(
            resolvedNativeCall,
            kNativeCallStub.data(),
            kNativeCallStub.size()) != 0 ||
        std::memcmp(
            resolvedHandleBase,
            kHandleBaseStub.data(),
            kHandleBaseStub.size()) != 0) {
        reason = "export-layout-mismatch";
        return std::nullopt;
    }

    return PinnedScriptHookModule{module, base};
}

struct NativeHandlerMemory final {
    const std::uint8_t* address{};
    std::string owner{"unknown"};
    std::uintptr_t ownerRelativeRva{};
    bool ownerRelative{};
};

[[nodiscard]] std::optional<NativeHandlerMemory>
InspectNativeHandlerMemory(const void* handler) {
    if (handler == nullptr) {
        return std::nullopt;
    }
    MEMORY_BASIC_INFORMATION memory{};
    if (::VirtualQuery(handler, &memory, sizeof(memory)) == 0U ||
        memory.State != MEM_COMMIT ||
        !IsExecutableProtection(memory.Protect)) {
        return std::nullopt;
    }
    const auto* address = static_cast<const std::uint8_t*>(handler);
    const auto* regionBegin =
        static_cast<const std::uint8_t*>(memory.BaseAddress);
    const auto* regionEnd = regionBegin + memory.RegionSize;
    if (address < regionBegin || address >= regionEnd ||
        static_cast<std::size_t>(regionEnd - address) <
            kAnimSceneHandlerPrologueBytes) {
        return std::nullopt;
    }

    NativeHandlerMemory result;
    result.address = address;
    const auto ownerModule =
        static_cast<HMODULE>(memory.AllocationBase);
    std::array<wchar_t, 32'768> path{};
    const auto length = ::GetModuleFileNameW(
        ownerModule,
        path.data(),
        static_cast<DWORD>(path.size()));
    if (length > 0U && length < path.size()) {
        const auto fileName = std::filesystem::path{
            std::wstring_view{path.data(), length}}.filename().wstring();
        result.owner = Utf8FromWide(fileName);
        if (result.owner.empty()) {
            result.owner = "module-name-unavailable";
        }
        const auto* ownerBase =
            reinterpret_cast<const std::uint8_t*>(ownerModule);
        if (address >= ownerBase) {
            result.ownerRelativeRva = static_cast<std::uintptr_t>(
                address - ownerBase);
            result.ownerRelative = true;
        }
    } else if (memory.Type == MEM_PRIVATE) {
        result.owner = "private-executable-memory";
    }
    return result;
}

[[nodiscard]] Vec3 ToBridgeVector(const Vector3& value) noexcept {
    // SDK Vector3 fields are individually ALIGN8; reading named fields avoids
    // the incorrect packed-float assumption.
    return {value.x, value.y, value.z};
}

[[nodiscard]] std::optional<float> ProbeGroundZ(
    const Vec3& position) noexcept {
    if (!IsFinite(position)) {
        return std::nullopt;
    }

    STREAMING::REQUEST_COLLISION_AT_COORD(
        position.x,
        position.y,
        position.z);
    constexpr std::array<float, 3> kProbeOffsets{
        2.0F,
        10.0F,
        50.0F};
    for (const auto offset : kProbeOffsets) {
        float groundZ{};
        if (GAMEPLAY::GET_GROUND_Z_FOR_3D_COORD(
                position.x,
                position.y,
                position.z + offset,
                &groundZ,
                FALSE) != FALSE &&
            std::isfinite(groundZ)) {
            return groundZ;
        }
    }
    return std::nullopt;
}

[[nodiscard]] Vec3 GroundSafePosition(
    const Vec3& position) noexcept {
    return SelectGroundSafePosition(position, ProbeGroundZ(position));
}

[[nodiscard]] Vec3 TeleportSafePosition(
    const Vec3& position) noexcept {
    if (!IsFinite(position)) {
        return position;
    }

    // GET_SAFE_COORD_FOR_PED is the engine's collision/navmesh-aware choice.
    // Keep it tightly bounded: an interior or unloaded remote area may return
    // a legitimate but distant coordinate, which is unsafe for co-op resume.
    STREAMING::REQUEST_COLLISION_AT_COORD(
        position.x,
        position.y,
        position.z);
    Vector3 safeCoordinate{};
    if (PATHFIND::GET_SAFE_COORD_FOR_PED(
            position.x,
            position.y,
            position.z,
            TRUE,
            &safeCoordinate,
            0) != FALSE) {
        const auto safe = ToBridgeVector(safeCoordinate);
        if (IsFinite(safe) && Distance(safe, position) <= 6.0F) {
            return safe;
        }
    }
    // Keep the existing height-only correction as a conservative fallback
    // while collision/navmesh data is still streaming.
    return GroundSafePosition(position);
}

[[nodiscard]] Vec3 CameraFallbackAimTarget() noexcept {
    const auto cameraPosition =
        ToBridgeVector(CAM::GET_GAMEPLAY_CAM_COORD());
    const auto cameraRotation =
        ToBridgeVector(CAM::GET_GAMEPLAY_CAM_ROT(2));
    if (!IsFinite(cameraPosition) ||
        !IsFinite(cameraRotation)) {
        return {};
    }

    const auto pitch =
        cameraRotation.x *
        (std::numbers::pi_v<float> / 180.0F);
    const auto yaw =
        cameraRotation.z *
        (std::numbers::pi_v<float> / 180.0F);
    const auto horizontal = std::abs(std::cos(pitch));
    const Vec3 direction{
        -std::sin(yaw) * horizontal,
        std::cos(yaw) * horizontal,
        std::sin(pitch)};
    return {
        cameraPosition.x +
            (direction.x * kFallbackAimDistanceMeters),
        cameraPosition.y +
            (direction.y * kFallbackAimDistanceMeters),
        cameraPosition.z +
            (direction.z * kFallbackAimDistanceMeters)};
}

void DrawNativeText(
    const std::string_view text,
    const float x,
    const float y,
    const float scale,
    const int red,
    const int green,
    const int blue,
    const int alpha = 255,
    const bool centered = false) {
    std::string owned{text};
    char literalString[] = "LITERAL_STRING";
    UI::SET_TEXT_SCALE(0.0F, scale);
    UI::SET_TEXT_COLOR_RGBA(red, green, blue, alpha);
    UI::SET_TEXT_CENTRE(centered ? TRUE : FALSE);
    UI::SET_TEXT_DROPSHADOW(1, 0, 0, 0, 220);
    UI::DRAW_TEXT(
        GAMEPLAY::CREATE_STRING(
            10,
            literalString,
            owned.data()),
        x,
        y);
}

void DrawNativeRectangle(
    const float x,
    const float y,
    const float width,
    const float height,
    const int red,
    const int green,
    const int blue,
    const int alpha) noexcept {
    GRAPHICS::DRAW_RECT(
        x,
        y,
        width,
        height,
        red,
        green,
        blue,
        alpha,
        FALSE,
        FALSE);
}

[[nodiscard]] float MoveTowards(
    const float current,
    const float target,
    const float maximumDelta) noexcept {
    if (!std::isfinite(current) ||
        !std::isfinite(target) ||
        !std::isfinite(maximumDelta) ||
        maximumDelta <= 0.0F) {
        return std::isfinite(target) ? target : 1.0F;
    }
    const auto difference = target - current;
    if (std::abs(difference) <= maximumDelta) {
        return target;
    }
    return current + std::copysign(maximumDelta, difference);
}

[[nodiscard]] float HorizontalDistance(
    const Vec3& lhs,
    const Vec3& rhs) noexcept {
    return std::hypot(lhs.x - rhs.x, lhs.y - rhs.y);
}

#endif

}  // namespace

[[nodiscard]] std::optional<CampaignMissionProbe>
ScriptHookSdkFacade::ProbeCampaignMission(
    const std::uint32_t expectedMissionId) noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        const auto definition = FindCampaignMission(expectedMissionId);
        if (!definition.has_value()) return std::nullopt;
        const auto missionId = static_cast<std::uint32_t>(GAMEPLAY::GET_HASH_KEY(
            const_cast<char*>(definition->scriptName.data())));
        const auto runtimeScriptId = static_cast<std::uint32_t>(
            GAMEPLAY::GET_HASH_KEY(
                const_cast<char*>(definition->runtimeScriptName.data())));
        if (missionId != definition->missionId || runtimeScriptId == 0U) {
            Log("[MISSION_PROGRESSION] mission catalog hash mismatch for " +
                std::string{definition->scriptName});
            return std::nullopt;
        }
        // The public API has no mission-specific "can start" native. Build
        // the strongest per-save preflight available from public MissionData:
        // the exact record must be valid, required Story content, incomplete
        // and unrated, in addition to RDR2 allowing this player to start a
        // mission at all. This rejects a later-chapter guest who has already
        // completed the offered mission instead of treating any open marker
        // as eligibility for it.
        const bool missionValid =
            ::invoke<BOOL>(0xE54DC27571D5EDC5ULL, missionId) != FALSE;
        const bool requiredStoryMission = missionValid &&
            ::invoke<BOOL>(0xE824CE7D13FCB35EULL, missionId) != FALSE;
        const bool wasCompleted = missionValid &&
            ::invoke<BOOL>(0xE54DC27571D5EDC4ULL, missionId) != FALSE;
        const std::uint8_t rating = missionValid
            ? static_cast<std::uint8_t>(std::clamp(
                ::invoke<int>(0x57E798B54C45EE1AULL, missionId), 0, 5))
            : static_cast<std::uint8_t>(0U);
        return CampaignMissionProbe{
            missionId,
            SCRIPT::_GET_NUMBER_OF_INSTANCES_OF_SCRIPT_WITH_NAME_HASH(
                runtimeScriptId) > 0,
            missionValid && requiredStoryMission && !wasCompleted &&
                rating == 0U &&
                PLAYER::CAN_PLAYER_START_MISSION(PLAYER::PLAYER_ID()) != FALSE,
            wasCompleted, rating};
#else
        (void)expectedMissionId;
#endif
    } catch (...) {
        Log("[MISSION_PROGRESSION] mission eligibility probe failed closed");
    }
    return std::nullopt;
}

bool ScriptHookSdkFacade::ApplyCampaignMissionCompletion(
    const std::uint32_t missionId,
    const std::uint64_t completionEventId,
    const std::uint8_t completionRating) noexcept {
    (void)completionEventId;
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
    try {
        constexpr auto kMissionRatingIncomplete = 0;
        const auto definition = FindCampaignMission(missionId);
        if (!definition.has_value() ||
            !HasVerifiedCampaignCompletionMapping(missionId)) {
            Log("[MISSION_PROGRESSION] completion mapping rejected: mission is not catalog-bound");
            return false;
        }
        // The header supplied for the pinned native study names these direct
        // public hashes as MISSIONDATA_IS_VALID, MISSIONDATA_GET_RATING,
        // MISSIONDATA_WAS_COMPLETED and _MISSIONDATA_SET_MISSION_RATING.
        if (::invoke<BOOL>(0xE54DC27571D5EDC5ULL, missionId) == FALSE) {
            Log("[MISSION_PROGRESSION] completion mapping rejected: MissionData id is invalid");
            return false;
        }
        const bool alreadyCompleted =
            ::invoke<BOOL>(0xE54DC27571D5EDC4ULL, missionId) != FALSE;
        const auto rating = ::invoke<int>(0x57E798B54C45EE1AULL, missionId);
        if (completionRating < 2U || completionRating > 5U ||
            (alreadyCompleted && rating != static_cast<int>(completionRating)) ||
            (!alreadyCompleted && rating != kMissionRatingIncomplete)) {
            Log("[MISSION_PROGRESSION] completion mapping rejected: guest MissionData state conflicts with host completion");
            return false;
        }
        if (!alreadyCompleted) {
            // The bundled ScriptHook SDK's invoke template cannot use void as
            // a return type. MissionData's setter has no meaningful result,
            // so receive its ABI-sized placeholder and deliberately discard it.
            (void)::invoke<Any>(
                0xE824CE7D13FCB300ULL, missionId,
                static_cast<int>(completionRating));
        }
        const bool completed =
            ::invoke<BOOL>(0xE54DC27571D5EDC4ULL, missionId) != FALSE &&
            ::invoke<int>(0x57E798B54C45EE1AULL, missionId) ==
                static_cast<int>(completionRating);
        if (!completed) {
            Log("[MISSION_PROGRESSION] MissionData completion verification failed");
            return false;
        }
        const auto playerPed = PLAYER::PLAYER_PED_ID();
        for (const auto& reward : CampaignMissionRewards(missionId)) {
            bool rewardApplied{};
            switch (reward.binding) {
                case CampaignMissionRewardBinding::WeaponOwnership: {
                    const auto weapon = static_cast<Hash>(reward.recordHash);
                    // The lasso is a permanent WNT4 reward but is a utility
                    // weapon; IS_WEAPON_VALID can report false for it during
                    // the prologue even though the same delayed-grant native
                    // accepts it. Keep this narrow exception tied to the
                    // explicit catalogue record rather than weakening the
                    // validation for arbitrary reward hashes.
                    const bool isKnownUtilityWeapon =
                        weapon == static_cast<Hash>(kWeaponLasso);
                    if (playerPed != 0 &&
                        (WEAPON::IS_WEAPON_VALID(weapon) != FALSE ||
                         isKnownUtilityWeapon)) {
                        if (WEAPON::HAS_PED_GOT_WEAPON(
                                playerPed, weapon, FALSE, FALSE) != FALSE) {
                            rewardApplied = true;
                        } else {
                            WEAPON::GIVE_DELAYED_WEAPON_TO_PED(
                                playerPed, weapon,
                                static_cast<int>(reward.amount), FALSE, 0);
                            rewardApplied = WEAPON::HAS_PED_GOT_WEAPON(
                                playerPed, weapon, FALSE, FALSE) != FALSE;
                        }
                    }
                    break;
                }
                case CampaignMissionRewardBinding::UnlockVisible: {
                    const auto unlock = static_cast<Hash>(reward.recordHash);
                    const bool visible =
                        UNLOCK::_0x8588A14B75AF096B(unlock) != FALSE;
                    if (!visible) {
                        UNLOCK::_0x46B901A8ECDB5A61(unlock, TRUE);
                    }
                    rewardApplied =
                        UNLOCK::_0x8588A14B75AF096B(unlock) != FALSE;
                    break;
                }
                case CampaignMissionRewardBinding::RecipeUnlock: {
                    // RDR2 stores a recipe's availability separately from
                    // the pamphlet held in inventory. The verified mission
                    // catalogue therefore models this as an explicit second
                    // reward record, rather than inferring it from a
                    // document grant.
                    const auto unlock = static_cast<Hash>(reward.recordHash);
                    const bool visible =
                        UNLOCK::_0x8588A14B75AF096B(unlock) != FALSE;
                    const bool unlocked =
                        UNLOCK::_0xC4B660C7B6040E75(unlock) != FALSE;
                    if (!visible) {
                        UNLOCK::_0x46B901A8ECDB5A61(unlock, TRUE);
                    }
                    if (!unlocked) {
                        UNLOCK::_0x1B7C5ADA8A6910A0(unlock, TRUE);
                    }
                    rewardApplied =
                        UNLOCK::_0x8588A14B75AF096B(unlock) != FALSE &&
                        UNLOCK::_0xC4B660C7B6040E75(unlock) != FALSE;
                    break;
                }
                case CampaignMissionRewardBinding::UnlockEntitlement: {
                    const auto unlock = static_cast<Hash>(reward.recordHash);
                    const bool visible =
                        UNLOCK::_0x8588A14B75AF096B(unlock) != FALSE;
                    const bool unlocked =
                        UNLOCK::_0xC4B660C7B6040E75(unlock) != FALSE;
                    if (!visible) {
                        UNLOCK::_0x46B901A8ECDB5A61(unlock, TRUE);
                    }
                    if (!unlocked) {
                        UNLOCK::_0x1B7C5ADA8A6910A0(unlock, TRUE);
                    }
                    rewardApplied =
                        UNLOCK::_0x8588A14B75AF096B(unlock) != FALSE &&
                        UNLOCK::_0xC4B660C7B6040E75(unlock) != FALSE;
                    break;
                }
                case CampaignMissionRewardBinding::WeaponShopEligibility: {
                    // Unlike a weapon ownership reward, this preserves the
                    // guest's economy: the item becomes buyable through the
                    // same local gunsmith path as vanilla Story Mode.
                    rewardApplied = UnlockLocalWeaponEntitlement(
                        reward.recordHash);
                    break;
                }
                case CampaignMissionRewardBinding::InventoryItem: {
                    // Story scripts create a CHARACTER parent GUID, resolve
                    // any required container GUID, resolve the item's own
                    // GUID/slot, then add the item. Follow that public-native
                    // sequence exactly; a raw item hash without its parent
                    // and compatible slot can corrupt or orphan inventory
                    // records.
                    constexpr Hash kDefaultInventorySlot = 1084182731U;
                    constexpr Hash kWardrobeInventorySlot = 1034665895U;
                    constexpr Hash kKitCampInventorySlot =
                        static_cast<Hash>(-1311702610);
                    constexpr Hash kAddReasonAwards = 0xB784AD1EU;
                    // joaat("CHARACTER") is deliberately calculated with
                    // the same stable hash routine used by the catalogue.
                    constexpr Hash kCharacter = static_cast<Hash>(
                        CampaignMissionId("CHARACTER"));
                    constexpr Hash kWardrobe = static_cast<Hash>(
                        CampaignMissionId("WARDROBE"));
                    constexpr Hash kKitCamp = static_cast<Hash>(
                        CampaignMissionId("KIT_CAMP"));
                    const auto item = static_cast<Hash>(reward.recordHash);
                    // These public inventory natives are named in the
                    // supplied header but absent from the older bundled SDK.
                    constexpr std::uint64_t kInventoryIdFromPed =
                        0x13D234A2A3F66E63ULL;
                    constexpr std::uint64_t kInventoryCountByItem =
                        0xE787F05DFC977BDEULL;
                    constexpr std::uint64_t kInventoryGuidFromItem =
                        0x886DFD3E185C8A89ULL;
                    constexpr std::uint64_t kInventoryGuidIsValid =
                        0xB881CA836CC4B6D4ULL;
                    constexpr std::uint64_t kInventoryFitsSlotId =
                        0x780C5B9AE2819807ULL;
                    constexpr std::uint64_t kInventoryAddWithGuid =
                        0xCB5D11F9508A928DULL;
                    constexpr std::uint64_t kItemDatabaseKeyValid =
                        0x6D5D51B188333FD1ULL;
                    if (::invoke<BOOL>(kItemDatabaseKeyValid, item, 0) ==
                        FALSE) {
                        break;
                    }
                    const auto inventoryId = ::invoke<int>(
                        kInventoryIdFromPed, PLAYER::PLAYER_PED_ID());
                    const auto before = inventoryId >= 0
                        ? ::invoke<int>(kInventoryCountByItem, inventoryId,
                            item, FALSE)
                        : -1;
                    if (inventoryId < 0 || before < 0 || reward.amount == 0U) {
                        break;
                    }
                    if (before > 0) {
                        rewardApplied = true;
                        break;
                    }
                    std::array<Any, 4U> emptyGuid{};
                    std::array<Any, 4U> characterGuid{};
                    std::array<Any, 4U> itemGuid{};
                    if (::invoke<BOOL>(kInventoryGuidFromItem, inventoryId,
                            emptyGuid.data(), kCharacter, 0,
                            characterGuid.data()) == FALSE ||
                        ::invoke<BOOL>(kInventoryGuidIsValid,
                            characterGuid.data()) == FALSE) {
                        break;
                    }
                    // This mirrors the standard Story helper's compatible
                    // slot selection. It makes documents, wardrobe records
                    // and camp-kit records distinct instead of forcing every
                    // reward into the CHARACTER/default slot.
                    auto itemSlot = kDefaultInventorySlot;
                    auto parentGuid = characterGuid;
                    if (::invoke<BOOL>(kInventoryFitsSlotId, item,
                            kDefaultInventorySlot) == FALSE) {
                        if (::invoke<BOOL>(kInventoryFitsSlotId, item,
                                kWardrobeInventorySlot) != FALSE) {
                            if (::invoke<BOOL>(kInventoryGuidFromItem,
                                    inventoryId, characterGuid.data(),
                                    kWardrobe, 0, parentGuid.data()) == FALSE ||
                                ::invoke<BOOL>(kInventoryGuidIsValid,
                                    parentGuid.data()) == FALSE) {
                                break;
                            }
                            itemSlot = kWardrobeInventorySlot;
                        } else if (::invoke<BOOL>(kInventoryFitsSlotId, item,
                                       kKitCampInventorySlot) != FALSE) {
                            if (::invoke<BOOL>(kInventoryGuidFromItem,
                                    inventoryId, characterGuid.data(),
                                    kKitCamp, 0, parentGuid.data()) == FALSE ||
                                ::invoke<BOOL>(kInventoryGuidIsValid,
                                    parentGuid.data()) == FALSE) {
                                break;
                            }
                            itemSlot = kKitCampInventorySlot;
                        } else {
                            Log("[MISSION_REWARD] inventory item has no supported Story slot");
                            break;
                        }
                    }
                    if (::invoke<BOOL>(kInventoryGuidFromItem, inventoryId,
                            parentGuid.data(), item, itemSlot,
                            itemGuid.data()) == FALSE ||
                        ::invoke<BOOL>(kInventoryAddWithGuid, inventoryId,
                            itemGuid.data(), parentGuid.data(), item, itemSlot,
                            static_cast<int>(reward.amount),
                            kAddReasonAwards) == FALSE) {
                        break;
                    }
                    const auto after =
                        ::invoke<int>(kInventoryCountByItem, inventoryId,
                            item, FALSE);
                    rewardApplied = after >= before +
                        static_cast<int>(reward.amount);
                    break;
                }
            }
            if (!rewardApplied) {
                Log("[MISSION_REWARD] catalog reward failed closed for " +
                    std::string{definition->scriptName} + ", record=" +
                    std::to_string(reward.recordHash));
                return false;
            }
        }
        // MissionData persists the actual completion and rating. Mark the
        // guest's vanilla mission journal only after every companion reward
        // has also succeeded, so a failed retry cannot advertise a completed
        // mission whose guest state is still incomplete. The documented UI
        // native is idempotent for an already-completed mission.
        (void)::invoke<Any>(0xDE31D66D1E54C471ULL, missionId);
        Log("[MISSION_PROGRESSION] MissionData completion mapping " +
            std::string{definition->scriptName} + " result=" +
            std::to_string(completed ? 1 : 0) + ", rating=" +
            std::to_string(completionRating));
        return true;
    } catch (...) {
        Log("[MISSION_PROGRESSION] MissionData completion mapping raised an exception");
    }
#else
    (void)missionId;
#endif
    return false;
}

std::optional<std::int32_t> ScriptHookSdkFacade::QueryLocalCashBalance()
    noexcept {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
    try {
        // MONEY::_MONEY_GET_CASH_BALANCE is public in the supplied native
        // header. It is sampled as a delta only; the total is never sent.
        const auto cash = ::invoke<int>(0x0C02DABFA3B98176ULL);
        return cash >= 0 ? std::optional<std::int32_t>{cash} : std::nullopt;
    } catch (...) {
        Log("[MISSION_REWARD] local cash snapshot failed");
    }
#endif
    return std::nullopt;
}

bool ScriptHookSdkFacade::ApplyCampaignMissionCashAward(
    const std::uint64_t completionEventId,
    const std::int32_t amount) noexcept {
    constexpr std::int32_t kMaximumMissionCashAward = 10'000'000;
    if (completionEventId == 0U || amount <= 0 ||
        amount > kMaximumMissionCashAward) {
        return false;
    }
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
    try {
        constexpr Hash kAddReasonAwards = 0xB784AD1EU;
        const auto before = ::invoke<int>(0x0C02DABFA3B98176ULL);
        if (before < 0 || ::invoke<BOOL>(
                0xBC3422DC91667621ULL, amount, kAddReasonAwards) == FALSE) {
            return false;
        }
        const auto after = ::invoke<int>(0x0C02DABFA3B98176ULL);
        const bool applied = after >= before && after - before == amount;
        Log("[MISSION_REWARD] cash event=" +
            std::to_string(completionEventId) + ", amount=" +
            std::to_string(amount) + ", result=" +
            std::to_string(applied ? 1 : 0));
        return applied;
    } catch (...) {
        Log("[MISSION_REWARD] guest cash award raised an exception");
    }
#else
    (void)completionEventId;
    (void)amount;
#endif
    return false;
}

ScriptHookSdkFacade::ScriptHookSdkFacade()
    : started_(std::chrono::steady_clock::now()) {}

void ScriptHookSdkFacade::AbandonNativeCleanupAfterFatal() noexcept {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
    RemoveAnimSceneCaptureHooks();
    animSceneHybridHandlersValidated_ = false;
    animSceneHybridResolvedHandlers_.fill(nullptr);
    animSceneHybridNativeCreationEnabled_ = false;
#endif
    abandonNativeCleanupAfterFatal_ = true;
}

void ScriptHookSdkFacade::InspectAnimSceneHybridHandlers(
    const int sceneHandle) noexcept {
    if (!animSceneHybridInspectorEnabled_ ||
        animSceneHybridInspectorAttempted_) {
        return;
    }
    animSceneHybridInspectorAttempted_ = true;
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        std::string rejectionReason;
        const auto scriptHook =
            FindPinnedScriptHookModule(rejectionReason);
        if (!scriptHook.has_value()) {
            Log(
                "[ANIMSCENE_HYBRID][INSPECTOR][DISABLED] capture=off, reason=" +
                rejectionReason +
                "; no memory was patched and SAFE_FALLBACK remains active");
            return;
        }

        Log(
            "[ANIMSCENE_HYBRID][CAPTURE][PREFLIGHT] mode=opt-in-exact-build, "
            "game=1.0.1491.50, scripthook=1.0.1491.17, scene-handle=" +
            std::to_string(sceneHandle) +
            "; resolving and pinning seven native handlers without nativeCall");

        const auto handlerSlot = reinterpret_cast<void* const*>(
            scriptHook->base + kPinnedScriptHookHandlerSlotRva);
        MEMORY_BASIC_INFORMATION handlerSlotMemory{};
        if (::VirtualQuery(
                handlerSlot,
                &handlerSlotMemory,
                sizeof(handlerSlotMemory)) == 0U ||
            handlerSlotMemory.State != MEM_COMMIT ||
            (handlerSlotMemory.Protect &
             (PAGE_GUARD | PAGE_NOACCESS)) != 0U ||
            reinterpret_cast<std::uintptr_t>(handlerSlot) +
                    sizeof(*handlerSlot) >
                reinterpret_cast<std::uintptr_t>(
                    handlerSlotMemory.BaseAddress) +
                    handlerSlotMemory.RegionSize) {
            Log(
                "[ANIMSCENE_HYBRID][INSPECTOR][DISABLED] capture=off, reason=handler-slot-not-readable; no memory was patched and SAFE_FALLBACK remains active");
            return;
        }
        std::size_t acceptedHandlers{};
        std::array<std::uint8_t*, 7U> resolvedHandlers{};
        for (std::size_t index = 0U;
             index < kAnimSceneNativeInspectionTargets.size();
             ++index) {
            const auto& target = kAnimSceneNativeInspectionTargets[index];
            // nativeInit only resolves and initializes ScriptHook's private
            // call context. Deliberately do not push arguments or call the
            // handler. Every ordinary SDK invoke starts with another
            // nativeInit, so the temporary context cannot leak into the next
            // bridge native call.
            nativeInit(target.hash);
            const auto* handler = *handlerSlot;
            const auto memory = InspectNativeHandlerMemory(handler);
            if (!memory.has_value() || !memory->ownerRelative ||
                memory->owner != "RDR2.exe" ||
                memory->ownerRelativeRva != target.expectedRva) {
                Log(
                    "[ANIMSCENE_HYBRID][INSPECTOR][HANDLER_REJECTED] native=" +
                    std::string{target.name} + ", hash=" +
                    HexValue(target.hash, 16U) +
                    ", reason=null-non-executable-or-unpinned-rva");
                continue;
            }

            ++acceptedHandlers;
            resolvedHandlers[index] = const_cast<std::uint8_t*>(
                memory->address);
            Log(
                "[ANIMSCENE_HYBRID][INSPECTOR][HANDLER] native=" +
                std::string{target.name} + ", hash=" +
                HexValue(target.hash, 16U) + ", signature=" +
                (target.signatureConfirmed ? "confirmed" : "candidate") +
                ", owner=" + memory->owner + ", rva=" +
                (memory->ownerRelative
                     ? HexValue(memory->ownerRelativeRva, 1U)
                     : std::string{"unavailable"}) +
                ", prologue32=" +
                HexBytes(
                    memory->address,
                    kAnimSceneHandlerPrologueBytes));
        }

        if (acceptedHandlers != kAnimSceneNativeInspectionTargets.size()) {
            animSceneHybridNativeCreationEnabled_ = false;
            Log(
                "[ANIMSCENE_HYBRID][CAPTURE][DISABLED] accepted=" +
                std::to_string(acceptedHandlers) + "/" +
                std::to_string(kAnimSceneNativeInspectionTargets.size()) +
                ", reason=incomplete-exact-handler-set; SAFE_FALLBACK retained");
            return;
        }
        animSceneHybridHandlersValidated_ = true;
        animSceneHybridResolvedHandlers_ = resolvedHandlers;
        animSceneHybridNativeCreationEnabled_ = true;
        if (animSceneCaptureHostAuthority_) {
            std::string hookError;
            if (!InstallAnimSceneCaptureHooks(
                    animSceneHybridResolvedHandlers_,
                    hookError)) {
                Log(
                    "[ANIMSCENE_HYBRID][CAPTURE][DISABLED] accepted=7/7, reason=" +
                    hookError +
                    "; partial hooks rolled back and SAFE_FALLBACK retained");
                return;
            }
            Log(
                "[ANIMSCENE_HYBRID][CAPTURE][ENABLED] role=host, accepted=7/7, detour=enabled, trampoline=register-preserving-indirect, capture=on, native-create=on; only bridge-owned guest scenes may be deleted");
        } else {
            RemoveAnimSceneCaptureHooks();
            Log(
                "[ANIMSCENE_HYBRID][CAPTURE][VALIDATED] role=guest, accepted=7/7, detour=disabled, capture=off, native-create=on; game-owned Story VM scenes remain untouched");
        }
#else
        (void)sceneHandle;
#endif
    } catch (...) {
        Log(
            "[ANIMSCENE_HYBRID][INSPECTOR][ERROR] read-only probe failed; "
            "capture stays off and SAFE_FALLBACK remains active");
    }
}

void ScriptHookSdkFacade::SetAnimSceneCaptureAuthority(
    const bool hostAuthority) noexcept {
    try {
        animSceneCaptureHostAuthority_ = hostAuthority;
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        if (!hostAuthority) {
            RemoveAnimSceneCaptureHooks();
            if (animSceneHybridHandlersValidated_ &&
                animSceneHybridInspectorEnabled_) {
                animSceneHybridNativeCreationEnabled_ = true;
                Log(
                    "[ANIMSCENE_HYBRID][CAPTURE][ROLE] guest native-create-only mode active; Story VM detours removed");
            }
            return;
        }
        if (!animSceneHybridInspectorEnabled_ ||
            !animSceneHybridHandlersValidated_) {
            return;
        }
        std::string hookError;
        if (!InstallAnimSceneCaptureHooks(
                animSceneHybridResolvedHandlers_,
                hookError)) {
            Log(
                "[ANIMSCENE_HYBRID][CAPTURE][DISABLED] role=host, reason=" +
                hookError +
                "; exact capture unavailable and SAFE_FALLBACK retained");
            return;
        }
        Log(
            "[ANIMSCENE_HYBRID][CAPTURE][ROLE] host authority confirmed; Story VM capture detours active");
#else
        (void)hostAuthority;
#endif
    } catch (...) {
        Log(
            "[ANIMSCENE_HYBRID][CAPTURE][ROLE][ERROR] could not apply role-scoped capture policy; capture remains fail-closed");
    }
}

bool ScriptHookSdkFacade::EnsureRemoteGunshotAudio() noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        if (!remoteGunshotSoundSetReady_) {
            remoteGunshotSoundSetReady_ =
                AUDIO::_0xD9130842D7226045(
                    reinterpret_cast<Any*>(
                        kRemoteGunshotSoundSet),
                    FALSE) != FALSE;
        }
        return remoteGunshotSoundSetReady_;
#endif
    } catch (...) {
        remoteGunshotSoundSetReady_ = false;
    }
    return false;
}

void ScriptHookSdkFacade::ReleaseRemoteGunshotAudio() noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        if (remoteGunshotSoundSetReady_) {
            AUDIO::_0x531A78D6BF27014B(
                reinterpret_cast<Any*>(
                    kRemoteGunshotSoundSet));
        }
#endif
    } catch (...) {
    }
    remoteGunshotSoundSetReady_ = false;
}

void ScriptHookSdkFacade::RestoreRemoteVisualFireAmmo(
    const LocalEntityHandle actor,
    const std::uint64_t nowMs,
    const bool force) noexcept {
    if (remoteVisualFireSuppressedWeaponHash_ == 0U ||
        (!force &&
         (remoteVisualFireRestoreAtMs_ == 0U ||
          nowMs < remoteVisualFireRestoreAtMs_))) {
        return;
    }
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        if (actor != 0 &&
            ENTITY::DOES_ENTITY_EXIST(actor) != FALSE) {
            const auto weapon = static_cast<Hash>(
                remoteVisualFireSuppressedWeaponHash_);
            const auto totalAmmo = static_cast<int>(
                std::min<std::uint32_t>(
                    remoteVisualFireRestoreAmmo_,
                    static_cast<std::uint32_t>(
                        std::numeric_limits<int>::max())));
            WEAPON::SET_PED_AMMO(actor, weapon, totalAmmo);
            if (remoteVisualFireRestoreClipKnown_) {
                const auto clipAmmo = static_cast<int>(
                    std::min<std::uint32_t>(
                        remoteVisualFireRestoreClipAmmo_,
                        remoteVisualFireRestoreAmmo_));
                (void)WEAPON::SET_AMMO_IN_CLIP(
                    actor,
                    weapon,
                    clipAmmo);
            }
            ++remoteActionFireAmmoRestores_;
        }
#else
        (void)actor;
        (void)nowMs;
        (void)force;
#endif
    } catch (...) {
        // EquipmentState will restore the authoritative total on its next
        // normal update even if the cosmetic pulse cleanup native failed.
    }
    remoteVisualFireSuppressedWeaponHash_ = 0U;
    remoteVisualFireRestoreAmmo_ = 0U;
    remoteVisualFireRestoreClipAmmo_ = 0U;
    remoteVisualFireRestoreAtMs_ = 0U;
    remoteVisualFireRestoreClipKnown_ = false;
}

bool ScriptHookSdkFacade::StartRemoteVisualFirePulse(
    const LocalEntityHandle actor,
    const Vec3& target,
    const std::uint64_t nowMs) noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        if (actor == 0 ||
            ENTITY::DOES_ENTITY_EXIST(actor) == FALSE ||
            !IsFinite(target) || remoteWeaponHash_ == 0U ||
            remoteWeaponHash_ == kWeaponUnarmed ||
            remoteWeaponHash_ == kWeaponLasso || remoteReloading_) {
            ++remoteActionFireGraphPulseSuppressed_;
            return false;
        }
        Hash currentWeapon{};
        if (WEAPON::GET_CURRENT_PED_WEAPON(
                actor,
                &currentWeapon,
                FALSE,
                0,
                FALSE) == FALSE ||
            currentWeapon != static_cast<Hash>(remoteWeaponHash_)) {
            ++remoteActionFireGraphPulseSuppressed_;
            return false;
        }

        if (remoteVisualFireSuppressedWeaponHash_ != 0U &&
            remoteVisualFireSuppressedWeaponHash_ != remoteWeaponHash_) {
            RestoreRemoteVisualFireAmmo(actor, nowMs, true);
        }
        if (remoteVisualFireSuppressedWeaponHash_ == 0U) {
            remoteVisualFireSuppressedWeaponHash_ = remoteWeaponHash_;
            remoteVisualFireRestoreAmmo_ = static_cast<std::uint32_t>(
                std::max(
                    WEAPON::GET_AMMO_IN_PED_WEAPON(
                        actor,
                        currentWeapon),
                    0));
            int clipAmmo{};
            remoteVisualFireRestoreClipKnown_ =
                WEAPON::GET_AMMO_IN_CLIP(
                    actor,
                    currentWeapon,
                    &clipAmmo) != FALSE;
            remoteVisualFireRestoreClipAmmo_ =
                remoteVisualFireRestoreClipKnown_
                    ? static_cast<std::uint32_t>(
                          std::max(clipAmmo, 0))
                    : 0U;
        }

        // TASK_SHOOT_AT_COORD is the only stable SDK task that advances the
        // weapon AnimGraph into a firing/recoil branch. Empty both total and
        // clip first, keep the task shorter than the ammo suppression lease,
        // and render the actual report/projectile separately at zero damage.
        // This gives the receiver a genuine weapon-graph pulse without ever
        // allowing the local AI task to create a damaging duplicate bullet.
        WEAPON::SET_PED_AMMO(actor, currentWeapon, 0);
        (void)WEAPON::SET_AMMO_IN_CLIP(actor, currentWeapon, 0);
        AI::TASK_SHOOT_AT_COORD(
            actor,
            target.x,
            target.y,
            target.z,
            kRemoteVisualFireTaskMilliseconds,
            static_cast<Hash>(kFiringPatternSingleShot),
            FALSE);
        PED::SET_PED_KEEP_TASK(actor, TRUE);
        remoteVisualFireRestoreAtMs_ =
            nowMs + kRemoteVisualFireAmmoRestoreMilliseconds;
        ++remoteActionFireGraphPulses_;
        return true;
#else
        (void)actor;
        (void)target;
        (void)nowMs;
#endif
    } catch (...) {
        ++remoteActionFireGraphPulseSuppressed_;
    }
    return false;
}

void ScriptHookSdkFacade::RestoreLocalLassoHogtieFlags() noexcept {
    if (!localLassoFlagsCaptured_) {
        return;
    }
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        constexpr std::array<int, 8> kManagedFlags{
            0, 2, 3, 4, 7, 8, 9, 11};
        const auto localPed = PLAYER::PLAYER_PED_ID();
        if (localPed != 0 &&
            ENTITY::DOES_ENTITY_EXIST(localPed) != FALSE) {
            for (std::size_t index{}; index < kManagedFlags.size(); ++index) {
                SetPedLassoHogtieFlag(
                    localPed,
                    kManagedFlags[index],
                    localLassoOriginalFlags_[index] ? TRUE : FALSE);
            }
        }
#endif
    } catch (...) {
    }
    localLassoOriginalFlags_.fill(false);
    localLassoFlagsCaptured_ = false;
}

void ScriptHookSdkFacade::DeleteRemotePeerLassoRope(
    const std::string_view reason) noexcept {
    const auto releasedActionId = remotePeerLassoRopeActionId_;
    const bool hadReplicatedConstraint =
        remotePeerLassoRope_ != 0 ||
        remotePeerLassoRopeActionId_ != 0U;
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        auto rope = static_cast<Object>(remotePeerLassoRope_);
        if (rope != 0 && ROPE::DOES_ROPE_EXIST(&rope) != FALSE) {
            ROPE::DELETE_ROPE(&rope);
        }
#endif
    } catch (...) {
    }
    RestoreLocalLassoHogtieFlags();
    if (hadReplicatedConstraint) {
        ++remotePlayerActionRopeDeletes_;
        Log("[LASSO_ROPE] released replicated native lasso constraint");
        Log(
            "[LASSO_LIFECYCLE] direction=rx, state=constraint-released, correlation=action-" +
            std::to_string(releasedActionId) +
            ", action-id=" + std::to_string(releasedActionId) +
            ", reason=" + std::string{reason});
    }
    remotePeerLassoRope_ = 0;
    remotePeerLassoRopeActionId_ = 0U;
    remotePeerLassoTaskStartedMs_ = 0U;
    remotePeerLassoTaskAttempts_ = 0U;
    remotePeerLassoTaskPending_ = false;
    remotePeerLassoEngineOwned_ = false;
}

void ScriptHookSdkFacade::RefreshRemotePlayerActionDerivedState() noexcept {
    const auto channelActive = [&](const PlayerActionKind kind) noexcept {
        const auto index = static_cast<std::size_t>(kind);
        return index < remotePlayerActionChannels_.size() &&
               remotePlayerActionChannels_[index].active;
    };
    remoteActionAimActive_ = channelActive(PlayerActionKind::Aim);
    remoteActionMeleeActive_ =
        channelActive(PlayerActionKind::MeleeAttack);
    remoteActionBlockActive_ =
        channelActive(PlayerActionKind::MeleeBlock);
    remoteActionGrappleActive_ =
        channelActive(PlayerActionKind::Grapple);
    // Begin visual ownership as soon as the authenticated throw transaction
    // exists. Waiting for PhysicalTargetEffect meant the receiver never ran
    // TASK_LASSO_PED until after the sender had already caught its local
    // proxy, so the remote screen could not show the wind-up, throw or rope.
    remoteActionLassoActive_ =
        channelActive(PlayerActionKind::Lasso) ||
        channelActive(PlayerActionKind::Hogtie);
    remoteActionKnockdownActive_ =
        channelActive(PlayerActionKind::Knockdown);
    remoteActionCraftingActive_ =
        channelActive(PlayerActionKind::Crafting);
}

void ScriptHookSdkFacade::CancelRemoteMeleeVisual(
    LocalEntityHandle actor,
    const std::string_view reason) noexcept {
    if (remoteMeleeVisualActionId_ == 0U) {
        return;
    }
    const auto cancelledActionId = remoteMeleeVisualActionId_;
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        if (actor == 0 && remotePlayerId_.IsValid()) {
            actor = replicas_.FindLocal(remotePlayerId_).value_or(0);
        }
        if (actor != 0 &&
            ENTITY::DOES_ENTITY_EXIST(actor) != FALSE &&
            PED::IS_PED_ON_MOUNT(actor) == FALSE) {
            // TASK_STAND_STILL(1) leaves the autonomous melee task alive in
            // parts of RDR2's combat graph. An immediate primary/secondary
            // cleanup is intentional at this transaction boundary; the
            // locomotion driver reacquires the proxy on its next transform.
            AI::CLEAR_PED_TASKS_IMMEDIATELY(actor, FALSE, TRUE);
            AI::CLEAR_PED_SECONDARY_TASK(actor);
            PED::SET_PED_KEEP_TASK(actor, FALSE);
            ++remotePlayerActionNativeCancels_;
        }
#else
        (void)actor;
#endif
    } catch (...) {
    }
    remoteMeleeVisualActionId_ = 0U;
    remoteMeleeVisualDeadlineMs_ = 0U;
    previousRemoteMeleeTaskMs_ = 0U;
    animGraphVisualTaskActive_ = false;
    animGraphVisualTaskStartedMs_ = 0U;
    Log(
        "[MELEE_VISUAL] one-shot owner released: action-id=" +
        std::to_string(cancelledActionId) +
        ", reason=" + std::string{reason});
    Log(
        "[COMBAT_LIFECYCLE] direction=rx, state=visual-owner-released, correlation=action-" +
        std::to_string(cancelledActionId) +
        ", action-id=" + std::to_string(cancelledActionId) +
        ", reason=" + std::string{reason});
}

void ScriptHookSdkFacade::MaintainPendingPeerDismount(
    const std::uint64_t nowMs) noexcept {
    if (remotePeerDismountActionId_ == 0U) {
        return;
    }
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        const auto localPed = PLAYER::PLAYER_PED_ID();
        if (localPed == 0 ||
            ENTITY::DOES_ENTITY_EXIST(localPed) == FALSE) {
            return;
        }
        if (PED::IS_PED_ON_MOUNT(localPed) == FALSE) {
            ++remotePlayerActionDismountConfirmed_;
            Log(
                "[PEER_DISMOUNT] victim-owned dismount confirmed by local ped state: action-id=" +
                std::to_string(remotePeerDismountActionId_) +
                ", latency-ms=" +
                std::to_string(
                    nowMs >= remotePeerDismountStartedMs_
                        ? nowMs - remotePeerDismountStartedMs_
                        : 0U));
            Log(
                "[MOUNT_LIFECYCLE] direction=rx, state=dismount-confirmed, correlation=action-" +
                std::to_string(remotePeerDismountActionId_) +
                ", action-id=" +
                std::to_string(remotePeerDismountActionId_) +
                ", reason=engine-state-confirmed");
            remotePeerDismountActionId_ = 0U;
            remotePeerDismountStartedMs_ = 0U;
            remotePeerDismountLastAttemptMs_ = 0U;
            remotePeerDismountAttempts_ = 0U;
            return;
        }
        const auto age =
            nowMs >= remotePeerDismountStartedMs_
                ? nowMs - remotePeerDismountStartedMs_
                : 0U;
        if (age >= kRemotePeerDismountTimeoutMilliseconds) {
            ++remotePlayerActionDismountFailed_;
            Log(
                "[WARNING][PEER_DISMOUNT] local engine did not confirm dismount before timeout: action-id=" +
                std::to_string(remotePeerDismountActionId_) +
                ", attempts=" +
                std::to_string(remotePeerDismountAttempts_));
            Log(
                "[MOUNT_LIFECYCLE] direction=rx, state=timeout, correlation=action-" +
                std::to_string(remotePeerDismountActionId_) +
                ", action-id=" +
                std::to_string(remotePeerDismountActionId_) +
                ", reason=engine-dismount-not-confirmed");
            remotePeerDismountActionId_ = 0U;
            remotePeerDismountStartedMs_ = 0U;
            remotePeerDismountLastAttemptMs_ = 0U;
            remotePeerDismountAttempts_ = 0U;
            return;
        }
        if (remotePeerDismountAttempts_ == 1U &&
            nowMs >= remotePeerDismountLastAttemptMs_ &&
            nowMs - remotePeerDismountLastAttemptMs_ >=
                kRemotePeerDismountRetryMilliseconds) {
            TaskDismountAnimal(localPed);
            remotePeerDismountLastAttemptMs_ = nowMs;
            remotePeerDismountAttempts_ = 2U;
            ++remotePlayerActionDismountRetries_;
            Log(
                "[WARNING][PEER_DISMOUNT] one controlled local retry issued: action-id=" +
                std::to_string(remotePeerDismountActionId_));
            Log(
                "[MOUNT_LIFECYCLE] direction=rx, state=retry, correlation=action-" +
                std::to_string(remotePeerDismountActionId_) +
                ", action-id=" +
                std::to_string(remotePeerDismountActionId_) +
                ", reason=engine-still-mounted");
        }
#else
        (void)nowMs;
#endif
    } catch (...) {
    }
}

void ScriptHookSdkFacade::ClearRemotePlayerActions() noexcept {
    CancelRemoteMeleeVisual(0, "session-reset");
    DeleteRemotePeerLassoRope();
    remotePlayerActionChannels_.fill(RemotePlayerActionChannel{});
    reliablePlayerActionProtocolObserved_ = false;
    remoteActionAimActive_ = false;
    remoteActionMeleeActive_ = false;
    remoteActionBlockActive_ = false;
    remoteActionGrappleActive_ = false;
    remoteActionLassoActive_ = false;
    remoteActionKnockdownActive_ = false;
    remoteActionCraftingActive_ = false;
    remotePeerDismountActionId_ = 0U;
    remotePeerDismountStartedMs_ = 0U;
    remotePeerDismountLastAttemptMs_ = 0U;
    remotePeerDismountAttempts_ = 0U;
}

void ScriptHookSdkFacade::ExpireRemotePlayerActions(
    const std::uint64_t nowMs) noexcept {
    if (!reliablePlayerActionProtocolObserved_) {
        return;
    }
    bool expiredMeleeOwner{};
    bool expiredAimOwner{};
    bool expiredLassoOwner{};
    for (std::size_t index = 1U;
         index < remotePlayerActionChannels_.size();
         ++index) {
        auto& channel = remotePlayerActionChannels_[index];
        if (!channel.active || channel.receivedAtMs == 0U ||
            nowMs < channel.receivedAtMs ||
            nowMs - channel.receivedAtMs <=
                kReliablePlayerActionTimeoutMilliseconds) {
            continue;
        }
        channel.active = false;
        channel.phase = PlayerActionPhase::Cancel;
        ++remotePlayerActionTimeouts_;
        const auto kind = static_cast<PlayerActionKind>(index);
        expiredAimOwner = expiredAimOwner ||
            kind == PlayerActionKind::Aim;
        expiredMeleeOwner = expiredMeleeOwner ||
            kind == PlayerActionKind::MeleeAttack ||
            kind == PlayerActionKind::Grapple;
        expiredLassoOwner = expiredLassoOwner ||
            kind == PlayerActionKind::Lasso ||
            kind == PlayerActionKind::Hogtie;
        Log(
            "[WARNING][ACTION_FSM] watchdog cancelled stale action kind=" +
            std::to_string(static_cast<std::uint8_t>(kind)) +
            ", action-id=" + std::to_string(channel.actionId) +
            ", age-ms=" +
            std::to_string(nowMs - channel.receivedAtMs));
        Log(
            std::string{
                kind == PlayerActionKind::Lasso ||
                        kind == PlayerActionKind::Hogtie
                    ? "[LASSO_LIFECYCLE]"
                    : "[COMBAT_LIFECYCLE]"} +
            " direction=rx, state=timeout, correlation=action-" +
            std::to_string(channel.actionId) +
            ", action-id=" + std::to_string(channel.actionId) +
            ", kind=" +
            std::to_string(static_cast<std::uint8_t>(kind)) +
            ", reason=reliable-refresh-timeout");
    }

    RefreshRemotePlayerActionDerivedState();

    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        const auto actor = replicas_.FindLocal(remotePlayerId_);
        if (actor.has_value() &&
            ENTITY::DOES_ENTITY_EXIST(*actor) != FALSE) {
            if (expiredMeleeOwner) {
                CancelRemoteMeleeVisual(
                    *actor,
                    "watchdog-timeout");
            }
            if (expiredAimOwner) {
                AI::CLEAR_PED_SECONDARY_TASK(*actor);
                previousRemoteAimTaskMs_ = 0U;
                animGraphVisualTaskActive_ = false;
                ++remotePlayerActionNativeCancels_;
            }
        }
#endif
    } catch (...) {
    }
    if (expiredLassoOwner && !remoteActionLassoActive_) {
        DeleteRemotePeerLassoRope();
    }
}

void ScriptHookSdkFacade::ResetRemoteMotionTracking() noexcept {
    latestRemoteAnimationState_.reset();
    latestRemoteAnimationStateReceivedAtMs_.reset();
    animGraphReplicaPreparedEntity_ = {};
    lastRemoteAnimationStateHash_ = 0U;
    lastRemoteAnimationGraphHash_ = 0U;
    animGraphVisualLocomotion_ = RemoteLocomotion::Idle;
    animGraphVisualDirection_ = RemoteMovementDirection::None;
    animGraphVisualTaskDestination_ = {};
    animGraphVisualTaskHeading_ = 0.0F;
    animGraphVisualTaskStartedMs_ = 0U;
    animGraphVisualTaskActive_ = false;
    animGraphReplicaTicks_ = 0U;
    animGraphReplicaCorrections_ = 0U;
    animGraphReplicaStateApplies_ = 0U;
    animGraphVisualTaskStarts_ = 0U;
    animGraphVisualTaskRefreshes_ = 0U;
    animGraphDirectionTransitions_ = 0U;
    animGraphTurnInPlaceTaskStarts_ = 0U;
    animGraphIkPreparations_ = 0U;
    animGraphExpectedMovingTicks_ = 0U;
    animGraphObservedMovingTicks_ = 0U;
    animGraphMissingLocomotionTicks_ = 0U;
    animGraphMissingLocomotionSinceMs_ = 0U;
    animGraphPreviousLocomotionRecoveryMs_ = 0U;
    animGraphLocomotionRecoveries_ = 0U;
    animGraphPreviousLocomotionMode_ =
        PlayerLocomotionMode::Grounded;
    animGraphLocomotionModeInitialized_ = false;
    animGraphTraversalExpectedTicks_ = 0U;
    animGraphTraversalObservedTicks_ = 0U;
    animGraphTraversalMissingTicks_ = 0U;
    animGraphTraversalJumpTaskStarts_ = 0U;
    animGraphTraversalClimbTaskStarts_ = 0U;
    animGraphAirborneLaunches_ = 0U;
    animGraphPhysicalRootYieldTicks_ = 0U;
    animGraphPhysicalRootLeashCorrections_ = 0U;
    animGraphRagdollTaskStarts_ = 0U;
    previousRemoteSemanticRagdollMs_ = 0U;
    animGraphStealthExpectedTicks_ = 0U;
    animGraphStealthObservedTicks_ = 0U;
    animGraphStealthTransitions_ = 0U;
    animGraphStealthMissingSinceMs_ = 0U;
    animGraphPreviousStealthRecoveryMs_ = 0U;
    animGraphStealthRecoveries_ = 0U;
    animGraphStealthActive_ = false;
    animGraphWaterExpectedTicks_ = 0U;
    animGraphWaterObservedTicks_ = 0U;
    animGraphSwimmingExpectedTicks_ = 0U;
    animGraphSwimmingObservedTicks_ = 0U;
    animGraphCoverActive_ = false;
    animGraphCoverFacingLeft_ = false;
    animGraphAimingFromCover_ = false;
    animGraphCoverFallbackCrouchActive_ = false;
    previousRemoteCoverTaskMs_ = 0U;
    animGraphCoverAcquireStartedMs_ = 0U;
    animGraphCoverAcquireRetries_ = 0U;
    animGraphCoverMissingSinceMs_ = 0U;
    animGraphPreviousCoverRecoveryMs_ = 0U;
    previousRemoteCoverFallbackAssertMs_ = 0U;
    animGraphCoverTransitions_ = 0U;
    animGraphCoverTaskStarts_ = 0U;
    animGraphCoverTaskCancels_ = 0U;
    animGraphCoverExpectedTicks_ = 0U;
    animGraphCoverObservedTicks_ = 0U;
    animGraphCoverFallbackStarts_ = 0U;
    animGraphCoverReacquires_ = 0U;
    animGraphCoverFallbackRecoveries_ = 0U;
    animGraphMeleeExpectedTicks_ = 0U;
    animGraphMeleeTaskStarts_ = 0U;
    animGraphMeleeMissingTargetTicks_ = 0U;
    animGraphMeleeTaskCancels_ = 0U;
    animGraphAimExpectedTicks_ = 0U;
    animGraphAimIdleTaskStarts_ = 0U;
    animGraphAimMovingTaskStarts_ = 0U;
    animGraphAimTaskCancels_ = 0U;
    animGraphAimTaskDestination_ = {};
    animGraphTraversalExpired_ = 0U;
    animGraphReplicaMoveNetworkSamples_ = 0U;
    animGraphReplicaUnavailableSamples_ = 0U;
    animGraphReplicaDiagnosticsStartedMs_ = 0U;
    animGraphReplicaPositionErrorSum_ = 0.0;
    animGraphReplicaPositionErrorMax_ = 0.0F;
    previousRemoteTransformMs_ = 0U;
    previousRemoteTaskMs_ = 0U;
    previousRemoteCoordinateCorrectionMs_ = 0U;
    previousRemoteTaskRecoveryMs_ = 0U;
    previousRemoteLocomotionChangeMs_ = 0U;
    previousRemoteTaskDestination_ = {};
    previousRemoteTargetPosition_ = {};
    remoteDiagnosticsTargetHeading_ = 0.0F;
    previousRemoteTaskHeading_ = 0.0F;
    previousRemoteLocomotion_ = RemoteLocomotion::Idle;
    hasPreviousRemoteTarget_ = false;
    hasRemoteLocomotionState_ = false;
    hasRemoteLocomotionTask_ = false;
    remoteMotionDiagnosticsStartedMs_ = 0U;
    remoteMotionApplyCount_ = 0U;
    remoteMotionMaximumApplyGapMs_ = 0U;
    remoteMotionGaitChanges_ = 0U;
    remoteMotionWarps_ = 0U;
    remoteMotionSoftCorrections_ = 0U;
    remoteMotionHardResyncs_ = 0U;
    remoteMotionEmergencyHardResyncs_ = 0U;
    remoteMotionHardResyncMaximumWaitMs_ = 0U;
    remoteMotionHardResyncMaximumDistance_ = 0.0F;
    remoteHardResyncErrorStartedMs_ = 0U;
    remoteHardResyncCooldownUntilMs_ = 0U;
    remoteMotionTaskRecoveries_ = 0U;
    remoteMotionAnimationTaskStarts_ = 0U;
    remoteMotionDestinationRefreshes_ = 0U;
    remoteMotionCatchUpTicks_ = 0U;
    remoteMotionDestinationHeadingTicks_ = 0U;
    remoteMotionPhysicsAssistTicks_ = 0U;
    remoteMotionVerticalAssistTicks_ = 0U;
    remoteMotionGroundedVerticalSuppressions_ = 0U;
    remoteMotionPhysicsInterruptedTicks_ = 0U;
    remoteMotionPhysicsInterruptionTransitions_ = 0U;
    remoteMotionPositionErrorSum_ = 0.0;
    remoteMotionPositionErrorMax_ = 0.0F;
    remoteMotionTargetGapMax_ = 0.0F;
    remoteMotionMaximumMoveRate_ = 1.0F;
    remoteMotionMaximumAssistSpeed_ = 0.0F;
    remoteMotionAssistVelocity_ = {};
    hasRemoteMotionAssistVelocity_ = false;
    remoteMotionPhysicsAssistActive_ = false;
    remoteMotionPhysicsInterrupted_ = false;
    remoteMotionAppliedMoveRate_ = 1.0F;
    remoteLocomotionEpoch_ = 0U;
    lastRemoteTraversalActionId_ = 0U;
    remoteControlMode_ = PuppetControlMode::GroundedLocomotion;
    hasRemoteControlMode_ = false;
    remoteControlModeTicks_.fill(0U);
    remoteControlModeTransitions_ = 0U;
    remoteMotionGroundedVelocitySuppressions_ = 0U;
    remoteRouteLookAheadSum_ = 0.0;
    remoteRouteLookAheadMaximum_ = 0.0F;
    remoteRouteCurvatureMaximum_ = 0.0F;
    remoteRouteLookAheadSamples_ = 0U;
    remoteTaskWatchdogCooldownMs_ =
        kRemoteTaskWatchdogInitialCooldownMilliseconds;
    remoteTaskWatchdogReissues_ = 0U;
    previousRemoteTaskWatchdogReissueMs_ = 0U;
    remoteWaypoints_.clear();
    previousRemoteNavigationProbeMs_ = 0U;
    remoteNavigationStalledSinceMs_ = 0U;
    remoteNavigationEnteredMs_ = 0U;
    remoteNavigationCooldownUntilMs_ = 0U;
    previousRemoteNavigationTaskMs_ = 0U;
    remoteNavigationDestinationWaypointCount_ = 0U;
    remoteNavigationProbePosition_ = {};
    remoteNavigationProbeTarget_ = {};
    remoteNavigationDestination_ = {};
    remoteNavigationProbeError_ = 0.0F;
    hasRemoteNavigationProbe_ = false;
    hasRemoteNavigationDestination_ = false;
    remoteNavigationActive_ = false;
    remoteNavigationEntries_ = 0U;
    remoteNavigationExits_ = 0U;
    remoteNavigationStalledSamples_ = 0U;
    remoteNavigationActiveTicks_ = 0U;
    remoteNavigationTaskStarts_ = 0U;
    remoteNavigationWaypointCaptures_ = 0U;
    remoteNavigationWaypointReached_ = 0U;
    remoteNavigationWaypointDrops_ = 0U;
    remoteNavigationExpiredWaypoints_ = 0U;
    remoteNavigationObsoleteWaypoints_ = 0U;
    remoteNavigationTrailResets_ = 0U;
    remoteNavigationTimeouts_ = 0U;
    remoteNavigationSafeRecoveryTeleports_ = 0U;
    remoteNavigationSafeRecoveryMaxDistance_ = 0.0F;
    remoteNavigationDirectTargetSelections_ = 0U;
    remoteNavigationPausedTicks_ = 0U;
    remoteNavigationSafeCoordHits_ = 0U;
    remoteNavigationSafeCoordMisses_ = 0U;
    remoteNavigationAssistSuppressedTicks_ = 0U;
    remoteNavigationMaximumQueue_ = 0U;
    remoteFireSequences_.Reset();
    previousRemoteAimTaskMs_ = 0U;
    previousRemoteAimTarget_ = {};
    previousRemoteMeleeTaskMs_ = 0U;
    localMeleeAttackLatchUntilMs_ = 0U;
    localMeleeBlockLatchUntilMs_ = 0U;
    localMeleeGrappleLatchUntilMs_ = 0U;
    localPeerLassoIntentUntilMs_ = 0U;
    localPeerMountPullLatchUntilMs_ = 0U;
    previousLocalMeleeAttackInput_ = false;
    lastRemoteAimTargetMs_ = 0U;
    lastRemoteAimTarget_ = {};
    pendingRemoteFireExpiresMs_ = 0U;
    pendingRemoteFireTarget_ = {};
    remoteAiming_ = false;
    remoteAimRootSuppressed_ = false;
    remoteMeleeCombat_ = false;
    remoteMeleeBlocking_ = false;
    ClearRemotePlayerActions();
    localPeerLassoLatched_ = false;
    remotePeerLassoActive_ = false;
    remotePeerKnockdownActive_ = false;
    previousPeerLassoRagdollMs_ = 0U;
    remoteJumping_ = false;
    remoteClimbing_ = false;
    pendingRemoteTraversals_.clear();
    remoteTraversalTaskGuardUntilMs_ = 0U;
    remoteActionDiagnosticsStartedMs_ = 0U;
    remoteActionAimTransitions_ = 0U;
    remoteActionAimTargetUpdates_ = 0U;
    remoteActionFireEvents_ = 0U;
    remoteActionVisualShots_ = 0U;
    remoteActionFireGraphPulses_ = 0U;
    remoteActionFireGraphPulseSuppressed_ = 0U;
    remoteActionFireAmmoRestores_ = 0U;
    remoteActionAudioShots_ = 0U;
    remoteActionAudioNotReady_ = 0U;
    remoteActionEquipmentUpdates_ = 0U;
    remoteActionWeaponGrants_ = 0U;
    remoteActionEquipmentSuppressed_ = 0U;
    remoteActionAimRootSuppressedTicks_ = 0U;
    remoteActionAimRootSuppressionTransitions_ = 0U;
    remoteActionJumpTaskStarts_ = 0U;
    remoteActionClimbTaskStarts_ = 0U;
    remoteActionTraversalDeferred_ = 0U;
    remoteActionTraversalExpired_ = 0U;
    remoteActionTraversalReliableUpdates_ = 0U;
    remoteActionTraversalGeometryConfirmed_ = 0U;
    remoteActionTraversalGeometryFallback_ = 0U;
    remotePlayerActionBegins_ = 0U;
    remotePlayerActionSustains_ = 0U;
    remotePlayerActionTerminals_ = 0U;
    remotePlayerActionPreemptions_ = 0U;
    remotePlayerActionStaleRevisions_ = 0U;
    remotePlayerActionNativeCancels_ = 0U;
    remotePlayerActionRopeCreates_ = 0U;
    remotePlayerActionRopeDeletes_ = 0U;
    remotePlayerActionTimeouts_ = 0U;
    remotePlayerActionLassoPending_ = 0U;
    remotePlayerActionLassoConfirmed_ = 0U;
    remotePlayerActionLassoFailed_ = 0U;
    remotePlayerActionVictimFallbacks_ = 0U;
    remotePlayerActionMotorYields_ = 0U;
    remotePlayerActionEpochRejects_ = 0U;
    remotePlayerActionForeignTerminals_ = 0U;
    remotePlayerActionMeleeVisualStarts_ = 0U;
    remotePlayerActionMeleeDeadlineCancels_ = 0U;
    remotePlayerActionMeleeSemanticOnly_ = 0U;
    remotePlayerActionDismountRequests_ = 0U;
    remotePlayerActionDismountRetries_ = 0U;
    remotePlayerActionDismountConfirmed_ = 0U;
    remotePlayerActionDismountFailed_ = 0U;
    remotePlayerActionDiagnosticsStartedMs_ = 0U;
    remoteWeaponNextGrantMs_ = 0U;
    previousRemoteWeaponVisualMs_ = 0U;
    remoteWeaponGrantAttempts_ = 0U;
    remoteWeaponConfirmedOwned_ = false;
    remoteWeaponAmmo_ = 0U;
    remoteVisualFireSuppressedWeaponHash_ = 0U;
    remoteVisualFireRestoreAmmo_ = 0U;
    remoteVisualFireRestoreClipAmmo_ = 0U;
    remoteVisualFireRestoreAtMs_ = 0U;
    remoteVisualFireRestoreClipKnown_ = false;
    remoteNicknameAnchor_ = {};
    hasRemoteNicknameAnchor_ = false;
}

void ScriptHookSdkFacade::CaptureRemoteWaypoint(
    const Vec3& position,
    const std::uint64_t nowMs) noexcept {
    if (!IsFinite(position)) {
        return;
    }
    while (!remoteWaypoints_.empty() &&
           nowMs >= remoteWaypoints_.front().capturedAtMs &&
           nowMs - remoteWaypoints_.front().capturedAtMs >
               kRemoteNavigationWaypointMaximumAgeMs) {
        remoteWaypoints_.pop_front();
        ++remoteNavigationExpiredWaypoints_;
        if (hasRemoteNavigationDestination_ &&
            remoteNavigationDestinationWaypointCount_ > 0U) {
            --remoteNavigationDestinationWaypointCount_;
            if (remoteNavigationDestinationWaypointCount_ == 0U) {
                hasRemoteNavigationDestination_ = false;
                previousRemoteNavigationTaskMs_ = 0U;
            }
        }
    }
    if (!remoteWaypoints_.empty() &&
        HorizontalDistance(
            remoteWaypoints_.back().position,
            position) <
            kRemoteNavigationWaypointSpacingMeters) {
        return;
    }
    if (remoteWaypoints_.size() >=
        kRemoteNavigationWaypointCapacity) {
        remoteWaypoints_.pop_front();
        ++remoteNavigationWaypointDrops_;
        if (hasRemoteNavigationDestination_ &&
            remoteNavigationDestinationWaypointCount_ > 0U) {
            --remoteNavigationDestinationWaypointCount_;
            if (remoteNavigationDestinationWaypointCount_ == 0U) {
                hasRemoteNavigationDestination_ = false;
                previousRemoteNavigationTaskMs_ = 0U;
            }
        }
    }
    remoteWaypoints_.push_back(RemoteWaypoint{position, nowMs});
    ++remoteNavigationWaypointCaptures_;
    remoteNavigationMaximumQueue_ = std::max(
        remoteNavigationMaximumQueue_,
        remoteWaypoints_.size());
}

void ScriptHookSdkFacade::ConsumeReachedRemoteWaypoints(
    const Vec3& currentPosition) noexcept {
    if (!IsFinite(currentPosition)) {
        return;
    }
    std::size_t closestIndex{};
    std::size_t inspected{};
    auto closestDistance =
        std::numeric_limits<float>::infinity();
    float inspectedRouteDistance{};
    std::optional<Vec3> previousInspected{};
    for (const auto& waypoint : remoteWaypoints_) {
        if (inspected >=
            kRemoteNavigationWaypointAnchorSearchCount) {
            break;
        }
        if (previousInspected.has_value()) {
            inspectedRouteDistance += HorizontalDistance(
                *previousInspected,
                waypoint.position);
            if (inspectedRouteDistance >
                kRemoteNavigationAnchorMaximumAdvanceMeters) {
                break;
            }
        }
        const auto distance = HorizontalDistance(
            currentPosition,
            waypoint.position);
        // Prefer the newest equally-close point. This is important when a
        // route doubles back near itself: the puppet advances instead of
        // selecting an older point behind its current progress.
        if (std::isfinite(distance) &&
            distance <= closestDistance) {
            closestDistance = distance;
            closestIndex = inspected;
        }
        previousInspected = waypoint.position;
        ++inspected;
    }
    if (closestIndex > 0U &&
        closestDistance <=
            kRemoteNavigationWaypointAnchorRadiusMeters) {
        for (std::size_t index = 0U;
             index < closestIndex;
             ++index) {
            remoteWaypoints_.pop_front();
            ++remoteNavigationObsoleteWaypoints_;
            if (hasRemoteNavigationDestination_ &&
                remoteNavigationDestinationWaypointCount_ > 0U) {
                --remoteNavigationDestinationWaypointCount_;
                if (remoteNavigationDestinationWaypointCount_ == 0U) {
                    hasRemoteNavigationDestination_ = false;
                    previousRemoteNavigationTaskMs_ = 0U;
                }
            }
        }
    }
    while (!remoteWaypoints_.empty() &&
           HorizontalDistance(
               currentPosition,
               remoteWaypoints_.front().position) <=
               kRemoteNavigationWaypointReachedMeters) {
        remoteWaypoints_.pop_front();
        ++remoteNavigationWaypointReached_;
        if (hasRemoteNavigationDestination_ &&
            remoteNavigationDestinationWaypointCount_ > 0U) {
            --remoteNavigationDestinationWaypointCount_;
            if (remoteNavigationDestinationWaypointCount_ == 0U) {
                hasRemoteNavigationDestination_ = false;
                previousRemoteNavigationTaskMs_ = 0U;
            }
        }
    }
}

std::optional<Vec3>
ScriptHookSdkFacade::SelectRemoteNavigationWaypoint() noexcept {
    if (remoteWaypoints_.empty()) {
        remoteNavigationDestinationWaypointCount_ = 0U;
        return std::nullopt;
    }
    auto selected = remoteWaypoints_.begin();
    std::size_t selectedCount{1U};
    float travelled{};
    for (auto iterator = std::next(remoteWaypoints_.begin());
         iterator != remoteWaypoints_.end();
         ++iterator) {
        travelled += HorizontalDistance(
            selected->position,
            iterator->position);
        selected = iterator;
        ++selectedCount;
        if (travelled >= kRemoteNavigationWaypointLookAheadMeters) {
            break;
        }
    }
    remoteNavigationDestinationWaypointCount_ = selectedCount;
    return selected->position;
}

std::optional<Vec3>
ScriptHookSdkFacade::SelectRemoteRouteWaypoint(
    const float lookAheadMeters) const noexcept {
    if (remoteWaypoints_.empty() ||
        !std::isfinite(lookAheadMeters)) {
        return std::nullopt;
    }
    auto selected = remoteWaypoints_.begin();
    float travelled{};
    for (auto iterator = std::next(remoteWaypoints_.begin());
         iterator != remoteWaypoints_.end();
         ++iterator) {
        travelled += HorizontalDistance(
            selected->position,
            iterator->position);
        selected = iterator;
        if (travelled >= std::max(0.0F, lookAheadMeters)) {
            break;
        }
    }
    return selected->position;
}

float ScriptHookSdkFacade::EstimateRemoteRouteCurvatureDegrees()
    const noexcept {
    if (remoteWaypoints_.size() < 3U) {
        return 0.0F;
    }
    const auto& first = remoteWaypoints_[0U].position;
    const auto& middle = remoteWaypoints_[1U].position;
    const auto& last = remoteWaypoints_[2U].position;
    const auto ax = middle.x - first.x;
    const auto ay = middle.y - first.y;
    const auto bx = last.x - middle.x;
    const auto by = last.y - middle.y;
    const auto aLength = std::hypot(ax, ay);
    const auto bLength = std::hypot(bx, by);
    if (aLength < 0.01F || bLength < 0.01F) {
        return 0.0F;
    }
    const auto cosine = std::clamp(
        ((ax * bx) + (ay * by)) / (aLength * bLength),
        -1.0F,
        1.0F);
    return std::acos(cosine) *
           (180.0F / std::numbers::pi_v<float>);
}

void ScriptHookSdkFacade::ClearRemoteIdentityDecoration() noexcept {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
    if (remoteBlip_ != 0) {
        auto blip = static_cast<Blip>(remoteBlip_);
        if (RADAR::DOES_BLIP_EXIST(blip) != FALSE) {
            RADAR::REMOVE_BLIP(&blip);
        }
        remoteBlip_ = 0;
    }
#endif
    remoteNickname_.clear();
    remoteNicknameAnchor_ = {};
    hasRemoteNicknameAnchor_ = false;
}

ScriptHookSdkFacade::~ScriptHookSdkFacade() {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
    RemoveAnimSceneCaptureHooks();
    animSceneHybridNativeCreationEnabled_ = false;
    if (abandonNativeCleanupAfterFatal_) {
        return;
    }
    MaintainMissionCompanionPresentation({});
    MaintainReplicatedMissionCamera(false, std::nullopt);
    (void)MaintainReplicatedAnimScene(false, std::nullopt);
    MaintainMissionSpectator(false);
    MaintainMissionResumeBarrier(false);
    (void)MaintainHostAnimSceneStartBarrier(false);
    MaintainRemoteMissionParticipant(false);
    ReleaseRemoteGunshotAudio();
    ClearRemotePlayerActions();
    if (synchronizedPauseActive_) {
        GAMEPLAY::SET_GAME_PAUSED(FALSE);
        GAMEPLAY::SET_TIME_SCALE(1.0F);
        synchronizedPauseActive_ = false;
    }
    if (worldClockWeatherOverrideActive_) {
        TIME::PAUSE_CLOCK(FALSE, 0);
        GAMEPLAY::FREEZE_WEATHER(FALSE);
        worldClockWeatherOverrideActive_ = false;
    }
    if (missionFlagOriginalCaptured_) {
        GAMEPLAY::SET_MISSION_FLAG(
            missionFlagOriginalValue_
                ? TRUE
                : FALSE);
        missionFlagOverrideActive_ = false;
        missionFlagOriginalCaptured_ = false;
    }
    CleanupWorldEntityProxies();
    RestoreHiddenAmbientPeds();
    ClearRemoteMount();
    ClearRemoteIdentityDecoration();
    for (const auto handle : replicas_.Drain()) {
        auto ped = static_cast<Ped>(handle);
        if (ped != 0 && ENTITY::DOES_ENTITY_EXIST(ped) != FALSE) {
            PED::DELETE_PED(&ped);
        }
    }
    if (remoteRelationshipGroup_ != 0U) {
        if (remoteRelationshipLocalGroup_ != 0U) {
            const auto localPed = PLAYER::PLAYER_PED_ID();
            if (localPed != 0 &&
                ENTITY::DOES_ENTITY_EXIST(localPed) != FALSE) {
                ENTITY::SET_ENTITY_CAN_BE_DAMAGED_BY_RELATIONSHIP_GROUP(
                    localPed,
                    TRUE,
                    remoteRelationshipGroup_);
            }
            PED::CLEAR_RELATIONSHIP_BETWEEN_GROUPS(
                5,
                remoteRelationshipLocalGroup_,
                remoteRelationshipGroup_);
            PED::CLEAR_RELATIONSHIP_BETWEEN_GROUPS(
                5,
                remoteRelationshipGroup_,
                remoteRelationshipLocalGroup_);
        }
        PED::REMOVE_RELATIONSHIP_GROUP(
            remoteRelationshipGroup_);
        remoteRelationshipGroup_ = 0U;
        remoteRelationshipLocalGroup_ = 0U;
    }
    remotePlayerId_ = NetEntityId{};
    remoteWeaponHash_ = 0U;
    remoteReloading_ = false;
    ResetRemoteMotionTracking();
#endif
}

std::uint64_t ScriptHookSdkFacade::TickMilliseconds() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_)
            .count());
}

RuntimeMode ScriptHookSdkFacade::QueryRuntimeMode() noexcept {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
    const bool online = NETWORK::NETWORK_IS_GAME_IN_PROGRESS() != FALSE;
    return {true, !online, online};
#else
    return {
        false,
        false,
        false};
#endif
}

std::optional<LocalPlayerSample>
ScriptHookSdkFacade::SampleLocalPlayer() noexcept {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
    const auto ped = PLAYER::PLAYER_PED_ID();
    if (ped == 0) {
        return std::nullopt;
    }
    auto health = static_cast<float>(ENTITY::GET_ENTITY_HEALTH(ped));
    const auto maximumHealth =
        static_cast<float>(ENTITY::GET_ENTITY_MAX_HEALTH(ped, FALSE));
    const auto rawHealthFraction =
        maximumHealth > 0.0F ? health / maximumHealth : 0.0F;
    const bool noTrackedPhysicalDamage =
        ENTITY::HAS_ENTITY_BEEN_DAMAGED_BY_ANY_PED(ped) == FALSE &&
        ENTITY::HAS_ENTITY_BEEN_DAMAGED_BY_ANY_OBJECT(ped) == FALSE &&
        ENTITY::HAS_ENTITY_BEEN_DAMAGED_BY_ANY_VEHICLE(ped) == FALSE;
    const bool localPedDeadOrDying =
        PED::IS_PED_DEAD_OR_DYING(ped, TRUE) != FALSE;
    if (realtimePolicyActive_ && guestHostMissionActive_ &&
        !guestHostPresentationActive_ && !localDownedPolicyActive_ &&
        maximumHealth > 0.0F &&
        (rawHealthFraction <= kGuestLocalHazardGuardHealthFraction ||
         localPedDeadOrDying) &&
        noTrackedPhysicalDamage) {
        // A guest can still have chapter-specific boundary/weather scripts
        // from its private save. They commonly write health/ragdoll directly
        // and therefore have no ped/object/vehicle damage owner. Intercept the
        // drain well above zero; the old 5% guard was too late for the Chapter
        // 1 "weather too harsh" script and the checkpoint reload had already
        // started. Physical ped/object/vehicle damage remains authoritative.
        if (localPedDeadOrDying || health <= 0.0F) {
            PED::RESURRECT_PED(ped);
            AI::CLEAR_PED_TASKS_IMMEDIATELY(
                ped,
                FALSE,
                TRUE);
        }
        health = std::max(
            1.0F,
            maximumHealth *
                kGuestLocalHazardRestoreHealthFraction);
        ENTITY::SET_ENTITY_HEALTH(
            ped,
            static_cast<int>(std::lround(health)),
            0);
        PED::RESET_PED_RAGDOLL_TIMER(ped);
        ++guestLocalHazardRecoveries_;
        Log(
            "[MISSION_ISOLATION][LOCAL_HAZARD_GUARD] rejected an unowned guest-save boundary/weather penalty during the host mission; restored 75% health");
    }
    LocalPlayerSample sample;
    sample.localHandle = static_cast<LocalEntityHandle>(ped);
    sample.position = ToBridgeVector(
        ENTITY::GET_ENTITY_COORDS(ped, TRUE, FALSE));
    sample.velocity = ToBridgeVector(
        ENTITY::GET_ENTITY_VELOCITY(ped, 0));
    sample.heading = ENTITY::GET_ENTITY_HEADING(ped);
    const auto desiredMoveBlend =
        AI::GET_PED_DESIRED_MOVE_BLEND_RATIO(ped);
    sample.desiredMoveBlendValid =
        std::isfinite(desiredMoveBlend) &&
        desiredMoveBlend >= 0.0F &&
        desiredMoveBlend <= 3.0F;
    sample.desiredMoveBlend =
        sample.desiredMoveBlendValid
            ? desiredMoveBlend
            : 0.0F;
    // The spectator barrier parks the real Story ped above its saved point so
    // it cannot enter the local mission trigger. That quarantine is strictly
    // local: publishing it made the host see the guest exactly 180 metres
    // away, fire the mission bubble and issue unnecessary rescue teleports.
    if (missionSpectatorActive_ &&
        IsFinite(missionSpectatorSavedPosition_) &&
        std::isfinite(missionSpectatorSavedHeading_)) {
        sample.position = missionSpectatorSavedPosition_;
        sample.velocity = {};
        sample.heading = missionSpectatorSavedHeading_;
        sample.desiredMoveBlendValid = true;
        sample.desiredMoveBlend = 0.0F;
    }
    sample.healthFraction =
        maximumHealth > 0.0F ? health / maximumHealth : 0.0F;
    sample.missionActive = GAMEPLAY::GET_MISSION_FLAG() != FALSE;
    sample.controlLocked =
        PLAYER::IS_PLAYER_CONTROL_ON(PLAYER::PLAYER_ID()) == FALSE &&
        UI::IS_PAUSE_MENU_ACTIVE() == FALSE;
    sample.screenTransition =
        CAM::IS_SCREEN_FADED_OUT() != FALSE ||
        CAM::IS_SCREEN_FADING_OUT() != FALSE ||
        CAM::IS_SCREEN_FADING_IN() != FALSE;
    sample.scenarioActive =
        PED::IS_PED_USING_ANY_SCENARIO(ped) != FALSE ||
        AI::IS_PED_ACTIVE_IN_SCENARIO(ped, TRUE) != FALSE;
    sample.vehicleEntryTransition =
        PED::IS_PED_GETTING_INTO_A_VEHICLE(ped) != FALSE;
    sample.minigameActive = GAMEPLAY::IS_MINIGAME_IN_PROGRESS() != FALSE;
    // Keep this signal limited to camera/fade presentation. Mission-owned
    // locks, AnimScenes, QTEs, and scripted mounting are classified and
    // debounced in BridgeRuntime, where they can be distinguished from a
    // normal free-roam interaction.
    sample.cutsceneActive =
        CAM::IS_CINEMATIC_CAM_RENDERING() != FALSE ||
        sample.screenTransition;
    const bool lethalGuardThresholdReached =
        realtimePolicyActive_ && maximumHealth > 0.0F &&
        health > 0.0F &&
        sample.healthFraction <= kDownedLethalGuardHealthFraction;
    // localDownedPolicyActive_ is an output latch maintained by the bridge,
    // not an engine observation. Feeding it back into sample.downed made the
    // lifecycle self-latching forever: even a newly respawned, healthy ped
    // could never satisfy the bridge's 1500 ms recovery confirmation.
    sample.downed =
        lethalGuardThresholdReached ||
        health <= 0.0F ||
        PED::IS_PED_DEAD_OR_DYING(ped, TRUE) != FALSE ||
        (realtimePolicyActive_ &&
         sample.healthFraction <= 0.08F);
    sample.mounted =
        PED::IS_PED_ON_MOUNT(ped) != FALSE;
    const auto now = TickMilliseconds();
    Ped ownedMount{};
    NetEntityId sharedMountEntityId{};
    std::uint32_t sharedMountGeneration{};
    bool borrowedPeerMount{};
    if (sample.mounted) {
        ownedMount = PED::GET_MOUNT(ped);
        if (ownedMount != 0 &&
            ENTITY::DOES_ENTITY_EXIST(ownedMount) != FALSE) {
            const auto peerMountId =
                remoteMountReplicas_.FindNetwork(
                    static_cast<LocalEntityHandle>(ownedMount));
            if (peerMountId.has_value()) {
                // Sitting on the peer's replicated horse does not transfer
                // ownership. Preserve its shared network identity so the
                // receiver attaches the rider to its existing local horse
                // instead of spawning a second horse at the same position.
                borrowedPeerMount = true;
                sharedMountEntityId = *peerMountId;
                sharedMountGeneration =
                    remoteMountGeneration_ == 0U
                        ? 1U
                        : remoteMountGeneration_;
            } else {
                localKnownMountHandle_ =
                    static_cast<LocalEntityHandle>(ownedMount);
                localKnownMountModelHash_ =
                    static_cast<std::uint32_t>(
                        ENTITY::GET_ENTITY_MODEL(ownedMount));
                localKnownMountConfirmedMs_ = now;
            }
        }
    }
    if (ownedMount == 0 &&
        localKnownMountHandle_ != 0) {
        const auto cached =
            static_cast<Ped>(localKnownMountHandle_);
        const bool sameCachedEntity =
            ENTITY::DOES_ENTITY_EXIST(cached) != FALSE &&
            localKnownMountModelHash_ != 0U &&
            static_cast<std::uint32_t>(
                ENTITY::GET_ENTITY_MODEL(cached)) ==
                localKnownMountModelHash_;
        const bool ownerConfirmed =
            sameCachedEntity &&
            CanPedBeMounted(cached) != FALSE &&
            GetPlayerOwnerOfMount(cached) ==
                PLAYER::PLAYER_ID();
        const bool continuityFresh =
            sameCachedEntity &&
            localKnownMountConfirmedMs_ != 0U &&
            now >= localKnownMountConfirmedMs_ &&
            now - localKnownMountConfirmedMs_ <=
                kLocalKnownMountContinuityMilliseconds;
        if (sameCachedEntity &&
            !remoteMountReplicas_.FindNetwork(
                 localKnownMountHandle_).has_value() &&
            !worldEntityReplicas_.FindNetwork(
                 localKnownMountHandle_).has_value() &&
            (ownerConfirmed || continuityFresh)) {
            ownedMount = cached;
            if (ownerConfirmed) {
                localKnownMountConfirmedMs_ = now;
            }
        } else {
            localKnownMountHandle_ = 0;
            localKnownMountModelHash_ = 0U;
            localKnownMountConfirmedMs_ = 0U;
        }
    }
    if (ownedMount == 0 &&
        (previousOwnedMountScanMs_ == 0U ||
         now < previousOwnedMountScanMs_ ||
         now - previousOwnedMountScanMs_ >=
             kOwnedMountScanMilliseconds)) {
        previousOwnedMountScanMs_ = now;
        std::array<int, kWorldPedPoolCapacity> peds{};
        const auto count = std::clamp(
            worldGetAllPeds(
                peds.data(),
                static_cast<int>(peds.size())),
            0,
            static_cast<int>(peds.size()));
        float nearestDistance =
            std::numeric_limits<float>::infinity();
        for (int index = 0; index < count; ++index) {
            const auto candidate = static_cast<Ped>(
                peds[static_cast<std::size_t>(index)]);
            if (candidate == 0 ||
                ENTITY::DOES_ENTITY_EXIST(candidate) == FALSE ||
                remoteMountReplicas_.FindNetwork(
                    static_cast<LocalEntityHandle>(candidate)).has_value() ||
                worldEntityReplicas_.FindNetwork(
                    static_cast<LocalEntityHandle>(candidate)).has_value() ||
                CanPedBeMounted(candidate) == FALSE ||
                GetPlayerOwnerOfMount(candidate) !=
                    PLAYER::PLAYER_ID()) {
                continue;
            }
            const auto candidatePosition = ToBridgeVector(
                ENTITY::GET_ENTITY_COORDS(
                    candidate,
                    TRUE,
                    FALSE));
            const auto candidateDistance =
                Distance(sample.position, candidatePosition);
            if (std::isfinite(candidateDistance) &&
                candidateDistance < nearestDistance) {
                nearestDistance = candidateDistance;
                ownedMount = candidate;
            }
        }
        if (ownedMount != 0) {
            localKnownMountHandle_ =
                static_cast<LocalEntityHandle>(ownedMount);
            localKnownMountModelHash_ =
                static_cast<std::uint32_t>(
                    ENTITY::GET_ENTITY_MODEL(ownedMount));
            localKnownMountConfirmedMs_ = now;
        }
    }
    if (ownedMount != 0 &&
        ENTITY::DOES_ENTITY_EXIST(ownedMount) != FALSE) {
        const auto mountHealth = static_cast<float>(
            ENTITY::GET_ENTITY_HEALTH(ownedMount));
        const auto mountMaximumHealth = static_cast<float>(
            ENTITY::GET_ENTITY_MAX_HEALTH(
                ownedMount,
                FALSE));
        auto mountHeading =
            ENTITY::GET_ENTITY_HEADING(ownedMount);
        mountHeading = std::fmod(mountHeading, 360.0F);
        if (mountHeading < 0.0F) {
            mountHeading += 360.0F;
        }
        const auto mountDead =
            mountHealth <= 0.0F ||
            PED::IS_PED_DEAD_OR_DYING(
                ownedMount,
                TRUE) != FALSE;
        LocalMountSample mount;
        mount.localHandle =
            static_cast<LocalEntityHandle>(ownedMount);
        mount.sharedEntityId = sharedMountEntityId;
        mount.modelHash = static_cast<std::uint32_t>(
            ENTITY::GET_ENTITY_MODEL(ownedMount));
        mount.sharedGeneration = sharedMountGeneration;
        mount.position = ToBridgeVector(
            ENTITY::GET_ENTITY_COORDS(
                ownedMount,
                TRUE,
                FALSE));
        mount.velocity = ToBridgeVector(
            ENTITY::GET_ENTITY_VELOCITY(
                ownedMount,
                0));
        mount.heading =
            std::isfinite(mountHeading)
                ? mountHeading
                : 0.0F;
        mount.healthFraction =
            mountMaximumHealth > 0.0F
                ? std::clamp(
                      mountHealth / mountMaximumHealth,
                      0.0F,
                      1.0F)
                : (mountDead ? 0.0F : 1.0F);
        mount.mounted =
            sample.mounted &&
            PED::GET_MOUNT(ped) == ownedMount;
        mount.dead = mountDead;
        mount.borrowedPeerMount = borrowedPeerMount;
        if (mount.modelHash != 0U &&
            IsFinite(mount.position) &&
            IsFinite(mount.velocity)) {
            // Keep the spectator staging offset out of the shared mount state
            // for the same reason as the player transform above.
            if (missionSpectatorActive_ &&
                IsFinite(missionSpectatorSavedPosition_) &&
                std::isfinite(missionSpectatorSavedHeading_)) {
                mount.position = missionSpectatorSavedPosition_;
                mount.velocity = {};
                mount.heading = missionSpectatorSavedHeading_;
            }
            sample.mount = mount;
        }
    }
    // A wagon is represented through the same relationship lane as a mount:
    // one endpoint owns the local physics (the driver) while the other is
    // assigned a seat in its own non-networked replica.  Never publish a
    // process-local vehicle handle.
    if (PED::IS_PED_IN_ANY_VEHICLE(ped, FALSE) != FALSE) {
        const auto vehicle = PED::GET_VEHICLE_PED_IS_IN(ped, FALSE);
        if (vehicle != 0 && ENTITY::DOES_ENTITY_EXIST(vehicle) != FALSE) {
            const auto localVehicleHandle =
                static_cast<LocalEntityHandle>(vehicle);
            const auto sharedVehicle =
                remoteVehicleReplicas_.FindNetwork(localVehicleHandle);
            const bool borrowedVehicle = sharedVehicle.has_value();
            const auto modelHash = static_cast<std::uint32_t>(
                ENTITY::GET_ENTITY_MODEL(vehicle));
            if (!borrowedVehicle) {
                localKnownVehicleHandle_ = localVehicleHandle;
                localKnownVehicleModelHash_ = modelHash;
            }
            LocalMountSample relationship;
            relationship.localHandle = localVehicleHandle;
            relationship.sharedEntityId =
                sharedVehicle.value_or(NetEntityId{});
            relationship.sharedGeneration = borrowedVehicle
                ? (remoteVehicleGeneration_ == 0U
                       ? 1U
                       : remoteVehicleGeneration_)
                : 0U;
            relationship.modelHash = modelHash;
            relationship.position = ToBridgeVector(
                ENTITY::GET_ENTITY_COORDS(vehicle, TRUE, FALSE));
            relationship.velocity = ToBridgeVector(
                ENTITY::GET_ENTITY_VELOCITY(vehicle, 0));
            auto vehicleHeading = ENTITY::GET_ENTITY_HEADING(vehicle);
            vehicleHeading = std::fmod(vehicleHeading, 360.0F);
            if (vehicleHeading < 0.0F) {
                vehicleHeading += 360.0F;
            }
            relationship.heading = vehicleHeading;
            const auto vehicleHealth = static_cast<float>(
                ENTITY::GET_ENTITY_HEALTH(vehicle));
            const auto vehicleMaximumHealth = static_cast<float>(
                ENTITY::GET_ENTITY_MAX_HEALTH(vehicle, FALSE));
            relationship.healthFraction = vehicleMaximumHealth > 0.0F
                ? std::clamp(vehicleHealth / vehicleMaximumHealth, 0.0F, 1.0F)
                : 1.0F;
            relationship.mounted = true;
            relationship.dead = vehicleHealth <= 0.0F;
            relationship.borrowedPeerMount = borrowedVehicle;
            relationship.vehicle = true;
            relationship.vehicleDriver =
                VEHICLE::GET_PED_IN_VEHICLE_SEAT(vehicle, -1) == ped;
            relationship.vehiclePassenger = !relationship.vehicleDriver;
            if (relationship.modelHash != 0U &&
                IsFinite(relationship.position) &&
                IsFinite(relationship.velocity)) {
                sample.mount = relationship;
            }
        }
    }
    sample.aiming =
        PLAYER::IS_PLAYER_FREE_AIMING(
            PLAYER::PLAYER_ID()) != FALSE;
    sample.firing =
        PED::IS_PED_SHOOTING(ped) != FALSE;
    const auto remotePlayerHandle =
        replicas_.FindLocal(remotePlayerId_);
    const auto meleeTarget = PED::GET_MELEE_TARGET_FOR_PED(ped);
    const bool validMeleeTarget =
        meleeTarget != 0 && meleeTarget != ped &&
        ENTITY::DOES_ENTITY_EXIST(meleeTarget) != FALSE;
    const bool meleeTargetsPeer =
        validMeleeTarget && remotePlayerHandle.has_value() &&
        meleeTarget == *remotePlayerHandle;
    bool peerCombatInRange{};
    if (remotePlayerHandle.has_value() &&
        ENTITY::DOES_ENTITY_EXIST(*remotePlayerHandle) != FALSE) {
        const auto peerPosition = ToBridgeVector(
            ENTITY::GET_ENTITY_COORDS(
                *remotePlayerHandle,
                TRUE,
                FALSE));
        peerCombatInRange =
            IsFinite(peerPosition) &&
            Distance(sample.position, peerPosition) <=
                kPeerMountPullMaximumDistanceMeters;
    }
    // Target acquisition alone also stays true while the player blocks. The
    // old sampler therefore told the other PC to punch on every snapshot,
    // stole the locomotion graph and made R look like another attack. Carry
    // input intent separately and keep a short attack latch so a 20 Hz stream
    // cannot miss a single click.
    const auto controlPressed = [](const int control) noexcept {
        return CONTROLS::IS_CONTROL_PRESSED(
                   kInputGroupGameplay,
                   control) != FALSE ||
               CONTROLS::IS_DISABLED_CONTROL_PRESSED(
                   kInputGroupGameplay,
                   control) != FALSE;
    };
    const auto controlJustPressed = [](const int control) noexcept {
        return CONTROLS::IS_CONTROL_JUST_PRESSED(
                   kInputGroupGameplay,
                   control) != FALSE ||
               CONTROLS::IS_DISABLED_CONTROL_JUST_PRESSED(
                   kInputGroupGameplay,
                   control) != FALSE;
    };
    sample.interactionHeld = std::any_of(
        kGuestStoryContextControls.begin(),
        kGuestStoryContextControls.end(),
        controlPressed);
    sample.interactionPressed = std::any_of(
        kGuestStoryContextControls.begin(),
        kGuestStoryContextControls.end(),
        controlJustPressed);
    const bool ownMountInteractionNearby =
        !sample.mounted && sample.mount.has_value() &&
        !sample.mount->borrowedPeerMount &&
        IsFinite(sample.mount->position) &&
        Distance(sample.position, sample.mount->position) <=
            kLocalOwnMountInteractionExclusionMeters;
    const bool rawMeleeAttackInput =
        controlPressed(kInputMeleeAttack) ||
        (meleeTargetsPeer &&
         controlPressed(kInputAttack)) ||
        controlPressed(kInputMeleeGrappleAttack);
    // RDR2 multiplexes the right-mouse context prompt with melee/grapple
    // controls. Treat that input as combat only after the context owner has
    // released it; otherwise merely looking at a talkable NPC can enable the
    // peer-combat isolation that calls SET_EVERYONE_IGNORE_PLAYER.
    const bool meleeAttackInput =
        rawMeleeAttackInput && !sample.interactionHeld &&
        !ownMountInteractionNearby;
    const bool meleeAttackInputEdge =
        meleeAttackInput &&
        (controlJustPressed(kInputMeleeAttack) ||
         (meleeTargetsPeer && controlJustPressed(kInputAttack)) ||
         controlJustPressed(kInputMeleeGrappleAttack) ||
         !previousLocalMeleeAttackInput_);
    const bool meleeBlockingInput =
        controlPressed(kInputMeleeBlock);
    const bool meleeGrapplingInput =
        controlPressed(kInputMeleeGrapple) ||
        controlPressed(kInputMeleeGrappleAttack);
    const bool meleeGrapplingInputEdge =
        meleeGrapplingInput &&
        !previousLocalPeerGrappleInput_;
    if (meleeBlockingInput) {
        localMeleeBlockLatchUntilMs_ =
            now + kLocalMeleeStateReleaseGraceMilliseconds;
    }
    if (meleeGrapplingInput) {
        localMeleeGrappleLatchUntilMs_ =
            now + kLocalMeleeStateReleaseGraceMilliseconds;
    }
    sample.meleeBlocking =
        localMeleeBlockLatchUntilMs_ != 0U &&
        now <= localMeleeBlockLatchUntilMs_;
    sample.meleeGrappling =
        localMeleeGrappleLatchUntilMs_ != 0U &&
        now <= localMeleeGrappleLatchUntilMs_;
    sample.meleeAttackPressed =
        meleeAttackInputEdge && !sample.meleeBlocking;
    if (sample.meleeAttackPressed) {
        localMeleeAttackLatchUntilMs_ =
            now + kLocalMeleeAttackLatchMilliseconds;
    }
    previousLocalMeleeAttackInput_ = meleeAttackInput;
    sample.meleeCombat =
        !sample.meleeBlocking &&
        localMeleeAttackLatchUntilMs_ != 0U &&
        now <= localMeleeAttackLatchUntilMs_;
    const bool remoteReplicaMounted =
        remotePlayerHandle.has_value() &&
        ENTITY::DOES_ENTITY_EXIST(*remotePlayerHandle) != FALSE &&
        PED::IS_PED_ON_MOUNT(*remotePlayerHandle) != FALSE;
    bool peerMountPullInRange{};
    if (remoteReplicaMounted) {
        const auto peerPosition = ToBridgeVector(
            ENTITY::GET_ENTITY_COORDS(
                *remotePlayerHandle,
                TRUE,
                FALSE));
        peerMountPullInRange =
            IsFinite(peerPosition) &&
            Distance(sample.position, peerPosition) <=
                kPeerMountPullMaximumDistanceMeters;
    }
    const bool peerMountPullInputEdge =
        ShouldPublishPeerMountPull(
            remoteReplicaMounted,
            peerMountPullInRange,
            meleeTargetsPeer,
            meleeAttackInputEdge,
            sample.interactionHeld,
            ownMountInteractionNearby);
    if (!peerMountPullInputEdge && remoteReplicaMounted &&
        peerMountPullInRange && meleeTargetsPeer && meleeAttackInputEdge &&
        (sample.interactionHeld || ownMountInteractionNearby)) {
        Log(
            "[MOUNT_INPUT_ISOLATION] ignored an overlapping context/melee edge while the local player was talking or mounting their own horse");
    }
    if (peerMountPullInputEdge) {
        localPeerMountPullLatchUntilMs_ =
            now + kLocalPeerMountPullLatchMilliseconds;
        Log(
            "[PEER_DISMOUNT] local mount-pull edge captured; publishing one victim-owned dismount transaction");
    }
    const bool peerMountPullActive =
        localPeerMountPullLatchUntilMs_ != 0U &&
        now <= localPeerMountPullLatchUntilMs_;
    const bool peerCombatInput =
        meleeAttackInputEdge || meleeGrapplingInput ||
        peerMountPullActive;
    sample.peerCombatTarget =
        meleeTargetsPeer || peerMountPullActive ||
        (peerCombatInRange && peerCombatInput);
    if (sample.peerCombatTarget && peerCombatInput) {
        localPeerCombatEffectUntilMs_ =
            now + kLocalPeerCombatEffectMemoryMilliseconds;
    }
    if (peerCombatInRange && meleeGrapplingInputEdge &&
        !sample.interactionHeld && !ownMountInteractionNearby) {
        // Paired grapple clips are selected only by the attacker's local RDR2
        // task graph. Publish a victim-owned knockdown immediately on the
        // deliberate grapple edge so the other PC cannot remain walking while
        // the attacker already sees a pin/tackle.
        sample.peerCombatTarget = true;
        localPeerKnockdownLatchUntilMs_ =
            now + kLocalPeerKnockdownLatchMilliseconds;
        Log(
            "[VICTIM_CONSTRAINT] close-range grapple edge captured; publishing deterministic victim knockdown");
    }
    previousLocalPeerGrappleInput_ = meleeGrapplingInput;

    const bool duckInput = controlPressed(kInputDuck);
    const bool duckInputEdge =
        controlJustPressed(kInputDuck) ||
        (duckInput && !previousLocalDuckInput_);
    const bool nativeStealth =
        PED::GET_PED_STEALTH_MOVEMENT(ped) != FALSE;
    const bool nativeCrouch =
        GetPedCrouchMovement(ped) != FALSE;
    if (duckInputEdge) {
        localStealthToggleLatched_ =
            !localStealthToggleLatched_;
    } else if (nativeStealth || nativeCrouch) {
        localStealthToggleLatched_ = true;
    } else if (sample.mounted ||
               (sample.desiredMoveBlendValid &&
                sample.desiredMoveBlend >= 2.5F)) {
        localStealthToggleLatched_ = false;
    }
    previousLocalDuckInput_ = duckInput;
    sample.stealthMovement =
        nativeCrouch || nativeStealth || localStealthToggleLatched_;
    const bool nativeInCover =
        PED::IS_PED_IN_COVER(ped, FALSE, FALSE) != FALSE;
    const bool nativeGoingIntoCover =
        PED::IS_PED_GOING_INTO_COVER(ped) != FALSE;
    if (nativeInCover || nativeGoingIntoCover) {
        // GOING_INTO_COVER may exist for less than one 20 Hz snapshot. Keep a
        // short semantic lease so the receiving PC always sees at least one
        // complete cover acquisition instead of a lost Q transition.
        localCoverSemanticUntilMs_ =
            now + kLocalCoverSemanticHoldMilliseconds;
    } else if (sample.mounted) {
        localCoverSemanticUntilMs_ = 0U;
    }
    const bool coverSemantic =
        nativeInCover || nativeGoingIntoCover ||
        (localCoverSemanticUntilMs_ != 0U &&
         now <= localCoverSemanticUntilMs_);
    sample.inCover = nativeInCover;
    sample.goingIntoCover =
        nativeGoingIntoCover ||
        (!nativeInCover && coverSemantic);
    sample.coverFacingLeft =
        sample.inCover &&
        PED::IS_PED_IN_COVER_FACING_LEFT(ped) != FALSE;
    sample.aimingFromCover =
        sample.inCover &&
        PED::IS_PED_AIMING_FROM_COVER(ped) != FALSE;
    if (sample.inCover || sample.goingIntoCover) {
        sample.stealthMovement = false;
    }
    if (coverSemantic != previousLocalCoverSemantic_) {
        previousLocalCoverSemantic_ = coverSemantic;
        Log(
            coverSemantic
                ? "[COVER_TX] local cover semantic entered (native/500ms edge lease)"
                : "[COVER_TX] local cover semantic released");
    }
    sample.jumping = PED::IS_PED_JUMPING(ped) != FALSE;
    sample.jumpPressed =
        CONTROLS::IS_CONTROL_JUST_PRESSED(
            kInputGroupGameplay,
            kInputJump) != FALSE;
    sample.climbing = PED::IS_PED_CLIMBING(ped) != FALSE;
    sample.falling = PED::IS_PED_FALLING(ped) != FALSE;
    sample.inWater = ENTITY::IS_ENTITY_IN_WATER(ped) != FALSE;
    sample.swimming = PED::IS_PED_SWIMMING(ped) != FALSE;
    sample.swimmingUnderwater =
        PED::IS_PED_SWIMMING_UNDER_WATER(ped) != FALSE;
    sample.ragdoll = PED::IS_PED_RAGDOLL(ped) != FALSE;
    sample.gettingUp = AI::IS_PED_GETTING_UP(ped) != FALSE;

    // Keep a forward capsule probe warm before the player presses jump. Shape
    // tests complete asynchronously, so probing only after IS_PED_CLIMBING
    // would discover the obstacle after the takeoff context had already gone.
    if (localTraversalProbeHandle_ != 0) {
        BOOL hit{};
        Vector3 hitPoint{};
        Vector3 hitNormal{};
        Entity hitEntity{};
        const auto status = SHAPETEST::GET_SHAPE_TEST_RESULT(
            localTraversalProbeHandle_,
            &hit,
            &hitPoint,
            &hitNormal,
            &hitEntity);
        if (status == 2) {
            localTraversalProbeHandle_ = 0;
            localTraversalObstacleValid_ = hit != FALSE;
            if (localTraversalObstacleValid_) {
                localTraversalObstaclePoint_ = ToBridgeVector(hitPoint);
                localTraversalObstacleNormal_ = ToBridgeVector(hitNormal);
                localTraversalObstacleTopZ_ =
                    localTraversalObstaclePoint_.z;
                localTraversalObstacleCapturedAtMs_ = now;
            }
        } else if (status == 0) {
            localTraversalProbeHandle_ = 0;
        }
    }
    if (!sample.mounted && localTraversalProbeHandle_ == 0 &&
        (previousLocalTraversalProbeMs_ == 0U ||
         now < previousLocalTraversalProbeMs_ ||
         now - previousLocalTraversalProbeMs_ >=
             kLocalTraversalProbeIntervalMs)) {
        const auto forward = ToBridgeVector(
            ENTITY::GET_ENTITY_FORWARD_VECTOR(ped));
        const Vec3 probeStart{
            sample.position.x,
            sample.position.y,
            sample.position.z + 0.45F};
        const Vec3 probeEnd{
            probeStart.x +
                (forward.x * kLocalTraversalProbeLengthMeters),
            probeStart.y +
                (forward.y * kLocalTraversalProbeLengthMeters),
            probeStart.z + 0.20F};
        localTraversalProbeHandle_ =
            SHAPETEST::START_SHAPE_TEST_CAPSULE(
                probeStart.x,
                probeStart.y,
                probeStart.z,
                probeEnd.x,
                probeEnd.y,
                probeEnd.z,
                kLocalTraversalProbeRadiusMeters,
                kTraversalShapeTestFlags,
                ped,
                7);
        previousLocalTraversalProbeMs_ = now;
    }
    const bool cachedTraversalObstacleFresh =
        localTraversalObstacleValid_ &&
        now >= localTraversalObstacleCapturedAtMs_ &&
        now - localTraversalObstacleCapturedAtMs_ <=
            kLocalTraversalProbeFreshnessMs &&
        Distance(sample.position, localTraversalObstaclePoint_) <= 3.0F;
    if (cachedTraversalObstacleFresh) {
        sample.traversalObstacleValid = true;
        sample.traversalObstaclePoint = localTraversalObstaclePoint_;
        sample.traversalObstacleNormal = localTraversalObstacleNormal_;
        sample.traversalObstacleTopZ = localTraversalObstacleTopZ_;
    }
    if (sample.meleeCombat) {
        if (validMeleeTarget) {
            sample.aimTarget = ToBridgeVector(
                ENTITY::GET_ENTITY_COORDS(
                    meleeTarget,
                    TRUE,
                    FALSE));
            sample.aimTarget.z += 0.75F;
            sample.aimTargetValid = IsFinite(sample.aimTarget);
        }
    }
    Entity aimedEntity{};
    if (!sample.aimTargetValid && (sample.aiming || sample.firing)) {
        if (PLAYER::GET_ENTITY_PLAYER_IS_FREE_AIMING_AT(
                PLAYER::PLAYER_ID(),
                &aimedEntity) != FALSE &&
            aimedEntity != 0 &&
            ENTITY::DOES_ENTITY_EXIST(aimedEntity) != FALSE) {
            sample.aimTarget = ToBridgeVector(
                ENTITY::GET_ENTITY_COORDS(
                    aimedEntity,
                    TRUE,
                    FALSE));
            sample.aimTarget.z += 0.75F;
            if (remotePlayerHandle.has_value() &&
                aimedEntity == *remotePlayerHandle) {
                sample.peerCombatTarget = true;
            }
        } else {
            sample.aimTarget = CameraFallbackAimTarget();
        }
        sample.aimTargetValid = IsFinite(sample.aimTarget);
        if (!sample.aimTargetValid) {
            sample.aimTarget = {};
        }
    }
    sample.reloading =
        PED::IS_PED_RELOADING(ped) != FALSE;
    Hash weaponHash{};
    if (WEAPON::GET_CURRENT_PED_WEAPON(
            ped,
            &weaponHash,
            TRUE,
            0,
            FALSE) != FALSE) {
        sample.weaponHash =
            static_cast<std::uint32_t>(weaponHash);
        sample.weaponAmmo =
            static_cast<std::uint32_t>(
                std::max(
                    WEAPON::GET_AMMO_IN_PED_WEAPON(
                        ped,
                        weaponHash),
                    0));
    }
    sample.weaponIsLasso =
        sample.weaponHash == kWeaponLasso;
    if (sample.weaponIsLasso) {
        sample.meleeCombat = false;
        sample.meleeGrappling = false;
    }
    const bool remoteReplicaRagdolled =
        remotePlayerHandle.has_value() &&
        ENTITY::DOES_ENTITY_EXIST(*remotePlayerHandle) != FALSE &&
        PED::IS_PED_RAGDOLL(*remotePlayerHandle) != FALSE;
    const bool remoteReplicaLassoed =
        remotePlayerHandle.has_value() &&
        ENTITY::DOES_ENTITY_EXIST(*remotePlayerHandle) != FALSE &&
        IsPedLassoed(static_cast<Ped>(*remotePlayerHandle)) != FALSE;
    const bool remoteReplicaBeingHogtied =
        remotePlayerHandle.has_value() &&
        ENTITY::DOES_ENTITY_EXIST(*remotePlayerHandle) != FALSE &&
        IsPedBeingHogtied(static_cast<Ped>(*remotePlayerHandle)) != FALSE;
    const bool remoteReplicaRestrained =
        remoteReplicaRagdolled || remoteReplicaLassoed ||
        remoteReplicaBeingHogtied;
    const bool remoteReplicaRagdollEdge =
        remoteReplicaRagdolled &&
        !previousRemoteReplicaRagdolled_;
    if (remoteReplicaRagdollEdge && peerCombatInRange &&
        localPeerCombatEffectUntilMs_ != 0U &&
        now <= localPeerCombatEffectUntilMs_) {
        localPeerKnockdownLatchUntilMs_ =
            now + kLocalPeerKnockdownLatchMilliseconds;
        sample.peerCombatTarget = true;
        Log(
            "[VICTIM_CONSTRAINT] local peer proxy entered ragdoll inside the recent combat window; publishing authoritative knockdown");
    }
    previousRemoteReplicaRagdolled_ = remoteReplicaRagdolled;
    // Do not treat merely aiming a lasso as a hit. The attacker's local RDR2
    // instance first has to put the target proxy into ragdoll; only then is
    // the restraint latched and forwarded. It stays latched if the hogtie
    // sequence temporarily changes the selected weapon, and clears when the
    // local target proxy is no longer physically restrained.
    if (sample.weaponHash == kWeaponLasso &&
        remoteReplicaRestrained) {
        localPeerLassoLatched_ = true;
    } else if (!remoteReplicaRestrained) {
        localPeerLassoLatched_ = false;
    }
    sample.peerLassoActive =
        localPeerLassoLatched_ &&
        remotePlayerHandle.has_value() &&
        remoteReplicaRestrained;
    const bool lassoThrowInput =
        sample.weaponIsLasso &&
        CONTROLS::IS_CONTROL_PRESSED(
            kInputGroupGameplay,
            kInputAttack) != FALSE;
    bool lassoAimCorridorTargetsPeer{};
    if (lassoThrowInput && !sample.peerCombatTarget &&
        aimedEntity == 0 && sample.aimTargetValid &&
        remotePlayerHandle.has_value() &&
        ENTITY::DOES_ENTITY_EXIST(*remotePlayerHandle) != FALSE) {
        auto peerAimPoint = ToBridgeVector(
            ENTITY::GET_ENTITY_COORDS(
                *remotePlayerHandle,
                TRUE,
                FALSE));
        peerAimPoint.z += 0.75F;
        const Vec3 ray{
            sample.aimTarget.x - sample.position.x,
            sample.aimTarget.y - sample.position.y,
            sample.aimTarget.z - sample.position.z};
        const Vec3 toPeer{
            peerAimPoint.x - sample.position.x,
            peerAimPoint.y - sample.position.y,
            peerAimPoint.z - sample.position.z};
        const auto rayLengthSquared =
            ray.x * ray.x + ray.y * ray.y + ray.z * ray.z;
        if (IsFinite(peerAimPoint) &&
            std::isfinite(rayLengthSquared) &&
            rayLengthSquared > 0.01F) {
            const auto rayLength = std::sqrt(rayLengthSquared);
            const Vec3 direction{
                ray.x / rayLength,
                ray.y / rayLength,
                ray.z / rayLength};
            const auto projection =
                toPeer.x * direction.x +
                toPeer.y * direction.y +
                toPeer.z * direction.z;
            const Vec3 closestOffset{
                toPeer.x - direction.x * projection,
                toPeer.y - direction.y * projection,
                toPeer.z - direction.z * projection};
            const auto corridorErrorSquared =
                closestOffset.x * closestOffset.x +
                closestOffset.y * closestOffset.y +
                closestOffset.z * closestOffset.z;
            lassoAimCorridorTargetsPeer =
                std::isfinite(projection) &&
                std::isfinite(corridorErrorSquared) &&
                projection > 0.25F &&
                projection <= kPeerLassoMaximumDistanceMeters &&
                corridorErrorSquared <=
                    kPeerLassoAimCorridorMeters *
                        kPeerLassoAimCorridorMeters;
        }
    }
    if (lassoAimCorridorTargetsPeer) {
        // RDR2 can report no aimed entity during the wind-up even though the
        // camera ray already crosses the peer. Name the peer on Begin so the
        // receiver does not start TASK_LASSO_PED several Sustain packets late.
        sample.peerCombatTarget = true;
    }
    if (lassoThrowInput) {
        localPeerLassoIntentUntilMs_ =
            now + kLocalPeerLassoIntentLatchMilliseconds;
    }
    sample.peerLassoIntent =
        sample.peerLassoActive ||
        (sample.weaponIsLasso &&
         localPeerLassoIntentUntilMs_ != 0U &&
         now <= localPeerLassoIntentUntilMs_);
    sample.peerKnockdown =
        peerMountPullActive ||
        (localPeerKnockdownLatchUntilMs_ != 0U &&
         now <= localPeerKnockdownLatchUntilMs_);
    sample.peerMountPull = peerMountPullActive;
    if (sample.peerKnockdown && peerCombatInRange) {
        sample.peerCombatTarget = true;
    }
    if (sample.peerLassoActive) {
        sample.peerCombatTarget = true;
        if (!sample.aimTargetValid) {
            sample.aimTarget = ToBridgeVector(
                ENTITY::GET_ENTITY_COORDS(
                    *remotePlayerHandle,
                    TRUE,
                    FALSE));
            sample.aimTarget.z += 0.75F;
            sample.aimTargetValid = IsFinite(sample.aimTarget);
        }
    }
    if ((sample.meleeCombat || sample.meleeBlocking ||
         sample.meleeGrappling || sample.peerLassoActive ||
         sample.peerKnockdown) &&
        sample.peerCombatTarget) {
        peerCombatIsolationUntilMs_ =
            now + kPeerCombatIsolationHoldMilliseconds;
    }
    return sample;
#else
    return std::nullopt;
#endif
}

std::optional<PlayerAppearanceStatePayload>
ScriptHookSdkFacade::SampleLocalAppearance(
    const NetEntityId entityId,
    const PlayerSlot slot,
    const std::uint32_t revision) noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        if (!entityId.IsValid() || revision == 0U) {
            return std::nullopt;
        }
        const auto ped = PLAYER::PLAYER_PED_ID();
        if (ped == 0 || ENTITY::DOES_ENTITY_EXIST(ped) == FALSE) {
            return std::nullopt;
        }
        const auto modelHash = static_cast<std::uint32_t>(
            ENTITY::GET_ENTITY_MODEL(ped));
        if (modelHash == 0U) {
            return std::nullopt;
        }

        std::vector<std::uint32_t> components;
        components.reserve(kMetaPedMaximumShopComponents);
        for (std::size_t index = 0U;
             index < kMetaPedMaximumShopComponents;
             ++index) {
            const auto component = static_cast<std::uint32_t>(
                GetShopItemComponentAtIndex(
                    ped,
                    static_cast<int>(index)));
            if (component == 0U ||
                std::find(
                    components.begin(),
                    components.end(),
                    component) != components.end()) {
                continue;
            }
            components.push_back(component);
        }
        if (components.empty()) {
            return std::nullopt;
        }

        // Stable FNV-1a over the model and the ordered shop-item list. The
        // fingerprint is only a change detector; it is never treated as a
        // pointer or as an engine identifier.
        std::uint64_t fingerprint = 1'469'598'103'934'665'603ULL;
        const auto mix = [&fingerprint](const std::uint32_t value) {
            for (unsigned int shift = 0U; shift < 32U; shift += 8U) {
                fingerprint ^= static_cast<std::uint8_t>(value >> shift);
                fingerprint *= 1'099'511'628'211ULL;
            }
        };
        mix(modelHash);
        for (const auto component : components) {
            mix(component);
        }
        if (fingerprint == 0U) {
            fingerprint = 1U;
        }

        return PlayerAppearanceStatePayload{
            entityId,
            slot,
            1U,
            static_cast<std::uint16_t>(
                PlayerAppearanceStateFlag::CompleteComponentSet) |
                static_cast<std::uint16_t>(
                    PlayerAppearanceStateFlag::StoryMetaPed),
            revision,
            modelHash,
            fingerprint,
            std::move(components)};
#else
        (void)entityId;
        (void)slot;
        (void)revision;
#endif
    } catch (...) {
        Log(
            "[ERROR][METAPED_APPEARANCE][TX] component enumeration failed safely");
    }
    return std::nullopt;
}

std::optional<AnimSceneReplicaStatePayload>
ScriptHookSdkFacade::SampleHostAnimScene(
    const NetEntityId hostEntityId,
    const std::uint32_t missionEpoch,
    const std::uint32_t cinematicGeneration,
    const std::uint32_t revision) noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        if (!hostEntityId.IsValid() || missionEpoch == 0U ||
            cinematicGeneration == 0U || revision == 0U) {
            return std::nullopt;
        }

        const auto sceneUsable = [](const int scene) {
            if (scene <= 0 || DoesAnimSceneExist(scene) == FALSE) {
                return false;
            }
            // Only a scene that currently owns an authored camera is a safe
            // transport candidate. Ambient/scenario AnimScenes can remain
            // running for minutes and must not pin the probe to the wrong
            // dictionary while a mission cutscene starts.
            return GetAnimSceneActiveCameraCount(scene) > 0;
        };
        if (!sceneUsable(hostAnimSceneHandle_)) {
            hostAnimSceneHandle_ = 0;
        }
        if (hostAnimSceneHandle_ == 0) {
            for (int attempt = 0;
                 attempt < kAnimSceneProbeBatchSize;
                 ++attempt) {
                if (hostAnimSceneProbeCursor_ <= 0 ||
                    hostAnimSceneProbeCursor_ >
                        kAnimSceneMaximumProbeHandle) {
                    hostAnimSceneProbeCursor_ = 1;
                }
                const auto candidate = hostAnimSceneProbeCursor_++;
                if (!sceneUsable(candidate)) {
                    continue;
                }
                const auto dictionary =
                    static_cast<std::uint32_t>(
                        GetAnimSceneDictionary(candidate));
                const auto duration =
                    GetAnimSceneDuration(candidate);
                if (dictionary == 0U || !std::isfinite(duration) ||
                    duration <= 0.0F || duration > 7'200.0F) {
                    continue;
                }
                hostAnimSceneHandle_ = candidate;
                break;
            }
        }
        if (hostAnimSceneHandle_ == 0) {
            return std::nullopt;
        }

        const auto dictionaryHash = static_cast<std::uint32_t>(
            GetAnimSceneDictionary(hostAnimSceneHandle_));
        const auto phase = GetAnimScenePhase(hostAnimSceneHandle_);
        const auto duration =
            GetAnimSceneDuration(hostAnimSceneHandle_);
        const auto rawRate = GetAnimSceneRate(hostAnimSceneHandle_);
        const auto running =
            IsAnimSceneRunning(hostAnimSceneHandle_) != FALSE;
        const auto loaded =
            IsAnimSceneLoaded(hostAnimSceneHandle_) != FALSE;
        const auto cameraCount = std::clamp(
            GetAnimSceneActiveCameraCount(hostAnimSceneHandle_),
            0,
            32);
        if (dictionaryHash == 0U || !std::isfinite(phase) ||
            phase < 0.0F || phase > 1.05F ||
            !std::isfinite(duration) || duration <= 0.0F ||
            duration > 7'200.0F || !std::isfinite(rawRate)) {
            hostAnimSceneHandle_ = 0;
            return std::nullopt;
        }

        Vector3 nativePosition{};
        Vector3 nativeRotation{};
        GetAnimSceneOrigin(
            hostAnimSceneHandle_,
            nativePosition,
            nativeRotation);
        const auto originPosition = ToBridgeVector(nativePosition);
        const auto originRotation = ToBridgeVector(nativeRotation);
        const bool originValid =
            IsFinite(originPosition) && IsFinite(originRotation);
        std::uint32_t flags = static_cast<std::uint32_t>(
            AnimSceneReplicaStateFlag::Active);
        if (running) {
            flags |= static_cast<std::uint32_t>(
                AnimSceneReplicaStateFlag::Running);
        }
        if (loaded) {
            flags |= static_cast<std::uint32_t>(
                AnimSceneReplicaStateFlag::Loaded);
        }
        if (cameraCount > 0) {
            flags |= static_cast<std::uint32_t>(
                AnimSceneReplicaStateFlag::CameraActive);
        }
        if (originValid) {
            flags |= static_cast<std::uint32_t>(
                AnimSceneReplicaStateFlag::OriginValid);
        }
        InspectAnimSceneHybridHandlers(hostAnimSceneHandle_);
        return AnimSceneReplicaStatePayload{
            hostEntityId,
            missionEpoch,
            cinematicGeneration,
            0U,
            revision,
            dictionaryHash,
            flags,
            phase,
            duration,
            std::clamp(rawRate, 0.0F, 4.0F),
            originValid ? originPosition : Vec3{},
            originValid ? originRotation : Vec3{},
            static_cast<std::uint16_t>(cameraCount)};
#else
        (void)hostEntityId;
        (void)missionEpoch;
        (void)cinematicGeneration;
        (void)revision;
#endif
    } catch (...) {
        hostAnimSceneHandle_ = 0;
        Log(
            "[ERROR][ANIMSCENE_REPLICA][TX] host scene probe failed safely; camera stream remains available");
    }
    return std::nullopt;
}

std::optional<LocalEntityHandle>
ScriptHookSdkFacade::SampledHostAnimSceneLocalHandle() noexcept {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
    return hostAnimSceneHandle_ > 0
               ? std::optional<LocalEntityHandle>{hostAnimSceneHandle_}
               : std::nullopt;
#else
    return std::nullopt;
#endif
}

std::vector<CapturedAnimSceneDefinition>
ScriptHookSdkFacade::DrainCapturedAnimSceneDefinitions() noexcept {
    std::vector<CapturedAnimSceneDefinition> result;
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        if (!animSceneHybridNativeCreationEnabled_) {
            return result;
        }
        auto captures = g_storyVmAnimSceneCapture.Drain();
        result.reserve(captures.size());
        for (auto& capture : captures) {
            std::uint32_t dictionaryHash{};
            float duration{};
            if (capture.sceneHandle > 0 &&
                DoesAnimSceneExist(capture.sceneHandle) != FALSE) {
                dictionaryHash = static_cast<std::uint32_t>(
                    GetAnimSceneDictionary(capture.sceneHandle));
                duration = GetAnimSceneDuration(capture.sceneHandle);
            }
            if (dictionaryHash == 0U || !std::isfinite(duration) ||
                duration <= 0.0F || duration > 7'200.0F) {
                Log(
                    "[ANIMSCENE_HYBRID][CAPTURE_REJECTED] completed Story VM record no longer has a valid dictionary/duration");
                continue;
            }

            for (auto& role : capture.roles) {
                const auto entity = static_cast<Entity>(role.localHandle);
                if (entity == 0 ||
                    ENTITY::DOES_ENTITY_EXIST(entity) == FALSE) {
                    role.localHandle = 0;
                    role.modelHash = 0U;
                    // Story scenes commonly release scene-local props
                    // immediately after START. Their captured role names are
                    // still useful to the guest resource, but there is no
                    // stable world handle to replicate. Keep actor-looking
                    // roles fail-closed while classifying the canonical RDR2
                    // prop/weapon prefixes as optional object bindings.
                    if (role.roleName.starts_with("p_") ||
                        role.roleName.starts_with("w_")) {
                        role.kind = AnimSceneRoleKind::Object;
                    }
                    continue;
                }
                role.modelHash = static_cast<std::uint32_t>(
                    ENTITY::GET_ENTITY_MODEL(entity));
                const auto entityType = ENTITY::GET_ENTITY_TYPE(entity);
                if (entityType == 1) {
                    const auto ped = static_cast<Ped>(entity);
                    role.kind = CanPedBeMounted(ped) != FALSE
                                    ? AnimSceneRoleKind::Horse
                                    : AnimSceneRoleKind::Ped;
                    if (PED::IS_PED_A_PLAYER(ped) != FALSE) {
                        role.flags |= static_cast<std::uint16_t>(
                            AnimSceneRoleFlag::Player);
                    }
                } else if (entityType == 2) {
                    role.kind = AnimSceneRoleKind::Vehicle;
                } else {
                    role.kind = AnimSceneRoleKind::Object;
                }
            }

            Log(
                "[ANIMSCENE_HYBRID][CAPTURED] sequence=" +
                std::to_string(capture.captureSequence) +
                ", scene=" + std::to_string(capture.sceneHandle) +
                ", resource=" + capture.resourceName +
                ", playback=" +
                (capture.playbackList.empty()
                     ? std::string{"<default>"}
                     : capture.playbackList) +
                ", scene-flags=" +
                std::to_string(capture.sceneFlags) +
                ", create-options=" +
                std::to_string(capture.createOptionFlags) +
                ", roles-at-load=" +
                std::to_string(capture.rolesAtLoad) +
                ", roles=" + std::to_string(capture.roles.size()));
            result.push_back(CapturedAnimSceneDefinition{
                capture.captureSequence,
                capture.sceneHandle,
                dictionaryHash,
                duration,
                capture.sceneFlags,
                capture.createOptionFlags,
                std::move(capture.resourceName),
                std::move(capture.playbackList),
                std::move(capture.roles),
                capture.complete});
        }
#endif
    } catch (...) {
        Log(
            "[ANIMSCENE_HYBRID][CAPTURE_REJECTED] exception while materializing captured roles; SAFE_FALLBACK retained");
    }
    return result;
}

std::optional<NetEntityId>
ScriptHookSdkFacade::FindKnownReplicaNetworkId(
    const LocalEntityHandle localHandle) noexcept {
    if (localHandle == 0) {
        return std::nullopt;
    }
    if (const auto player = replicas_.FindNetwork(localHandle);
        player.has_value()) {
        return player;
    }
    if (const auto mount = remoteMountReplicas_.FindNetwork(localHandle);
        mount.has_value()) {
        return mount;
    }
    return worldEntityReplicas_.FindNetwork(localHandle);
}

std::optional<MissionCameraSample>
ScriptHookSdkFacade::SampleMissionCamera() noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        MissionCameraSample sample;
        const auto renderingCamera = CAM::GET_RENDERING_CAM();
        int sampleSource{};
        if (renderingCamera != 0 &&
            CAM::DOES_CAM_EXIST(renderingCamera) != FALSE &&
            CAM::IS_CAM_RENDERING(renderingCamera) != FALSE) {
            sampleSource = 2;
            sample.position = ToBridgeVector(
                CAM::GET_CAM_COORD(renderingCamera));
            sample.rotation = ToBridgeVector(
                CAM::GET_CAM_ROT(renderingCamera, 2));
            sample.fieldOfView =
                CAM::GET_CAM_FOV(renderingCamera);
        } else {
            sampleSource =
                CAM::IS_CINEMATIC_CAM_RENDERING() != FALSE
                    ? 1
                    : 0;
            sample.position = ToBridgeVector(
                CAM::GET_GAMEPLAY_CAM_COORD());
            sample.rotation = ToBridgeVector(
                CAM::GET_GAMEPLAY_CAM_ROT(2));
            sample.fieldOfView =
                CAM::GET_GAMEPLAY_CAM_FOV();
        }
        if (sampleSource != missionCameraSampleSource_) {
            missionCameraSampleSource_ = sampleSource;
            Log(
                sampleSource == 2
                    ? "[MISSION_CAMERA][SAMPLE] source=rendering-script-camera"
                    : sampleSource == 1
                          ? "[MISSION_CAMERA][SAMPLE] source=cinematic-gameplay-camera"
                          : "[MISSION_CAMERA][SAMPLE] source=gameplay-camera-fallback");
        }
        sample.flags |= static_cast<std::uint32_t>(
            sampleSource == 2
                ? MissionCameraStateFlag::SourceRenderingScriptCamera
                : sampleSource == 1
                      ? MissionCameraStateFlag::SourceCinematicGameplayCamera
                      : MissionCameraStateFlag::SourceGameplayCameraFallback);
        if (!IsFinite(sample.position) ||
            !IsFinite(sample.rotation) ||
            !std::isfinite(sample.fieldOfView) ||
            sample.fieldOfView < 1.0F ||
            sample.fieldOfView > 179.0F) {
            return std::nullopt;
        }
        if (CAM::IS_SCREEN_FADED_OUT() != FALSE) {
            sample.flags |= static_cast<std::uint32_t>(
                MissionCameraStateFlag::ScreenFadedOut);
        } else if (CAM::IS_SCREEN_FADING_OUT() != FALSE) {
            sample.flags |= static_cast<std::uint32_t>(
                MissionCameraStateFlag::ScreenFadingOut);
        } else if (CAM::IS_SCREEN_FADING_IN() != FALSE) {
            sample.flags |= static_cast<std::uint32_t>(
                MissionCameraStateFlag::ScreenFadingIn);
        }
        return sample;
#endif
    } catch (...) {
        Log(
            "[ERROR][MISSION_CAMERA][TX] failed to sample the rendered host camera");
    }
    return std::nullopt;
}

std::optional<PlayerAnimationStatePayload>
ScriptHookSdkFacade::SampleLocalAnimationState(
    const NetEntityId entityId,
    const PlayerSlot slot,
    const std::uint16_t locomotionEpoch,
    const std::uint32_t sampleSequence) noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        if (motionReplicationMode_ !=
                MotionReplicationWireMode::AnimGraphReplica ||
            !entityId.IsValid() || locomotionEpoch == 0U) {
            return std::nullopt;
        }

        const auto ped = PLAYER::PLAYER_PED_ID();
        if (ped == 0 || ENTITY::DOES_ENTITY_EXIST(ped) == FALSE) {
            return std::nullopt;
        }

        PlayerAnimationStatePayload sample{
            entityId,
            slot,
            kPlayerAnimationStateSchemaVersion,
            PlayerAnimationSampleSource::None,
            locomotionEpoch,
            sampleSequence};

        // The SDK cannot enumerate the active locomotion graph's clips. Read
        // the deepest stable native state that is actually available and mark
        // it explicitly as LocomotionNative rather than pretending it came
        // from the versioned memory reader. A scripted move network is common
        // in camps and missions; it must not suppress this safe native sample
        // or the receiver keeps an old Idle state and slides the replica.
        const auto desiredMoveBlend =
            AI::GET_PED_DESIRED_MOVE_BLEND_RATIO(ped);
        const bool desiredMoveBlendValid =
            std::isfinite(desiredMoveBlend) &&
            desiredMoveBlend >= 0.0F &&
            desiredMoveBlend <= 3.0F;
        std::uint32_t motionState = kMotionStateIdle;
        if (AI::IS_PED_SPRINTING(ped) != FALSE ||
            AI::IS_MOVE_BLEND_RATIO_SPRINTING(ped) != FALSE ||
            (desiredMoveBlendValid && desiredMoveBlend >= 2.5F)) {
            motionState = kMotionStateSprint;
        } else if (
            AI::IS_PED_RUNNING(ped) != FALSE ||
            AI::IS_MOVE_BLEND_RATIO_RUNNING(ped) != FALSE ||
            (desiredMoveBlendValid && desiredMoveBlend >= 1.5F)) {
            motionState = kMotionStateRun;
        } else if (
            AI::IS_PED_WALKING(ped) != FALSE ||
            AI::IS_MOVE_BLEND_RATIO_WALKING(ped) != FALSE ||
            (desiredMoveBlendValid && desiredMoveBlend >= 0.05F)) {
            motionState = kMotionStateWalk;
        }
        sample.source =
            PlayerAnimationSampleSource::LocomotionNative;
        sample.capabilities =
            static_cast<std::uint32_t>(
                PlayerAnimationCapability::StateIdentifier) |
            static_cast<std::uint32_t>(
                PlayerAnimationCapability::RuntimeFlags);
        sample.flags =
            static_cast<std::uint32_t>(
                PlayerAnimationStateFlag::StateHashValid) |
            static_cast<std::uint32_t>(
                PlayerAnimationStateFlag::Looping);
        sample.stateHash = motionState;
        return sample;
#else
        (void)entityId;
        (void)slot;
        (void)locomotionEpoch;
        (void)sampleSequence;
        return std::nullopt;
#endif
    } catch (...) {
        ++animGraphReplicaUnavailableSamples_;
        return std::nullopt;
    }
}

std::optional<WorldStatePayload>
ScriptHookSdkFacade::SampleWorldState() noexcept {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
    WorldStatePayload state;
    const auto hour = TIME::GET_CLOCK_HOURS();
    const auto minute = TIME::GET_CLOCK_MINUTES();
    const auto second = TIME::GET_CLOCK_SECONDS();
    const auto day = TIME::GET_CLOCK_DAY_OF_MONTH();
    const auto month = TIME::GET_CLOCK_MONTH();
    const auto year = TIME::GET_CLOCK_YEAR();
    if (hour < 0 || hour > 23 ||
        minute < 0 || minute > 59 ||
        second < 0 || second > 59 ||
        day < 1 || day > 31 ||
        month < 0 || month > 11 ||
        year < 1800 || year > 2200) {
        return std::nullopt;
    }
    state.hour = static_cast<std::uint8_t>(hour);
    state.minute = static_cast<std::uint8_t>(minute);
    state.second = static_cast<std::uint8_t>(second);
    state.day = static_cast<std::uint8_t>(day);
    state.month = static_cast<std::uint8_t>(month);
    state.year = static_cast<std::uint16_t>(year);

    Hash weatherFrom{};
    Hash weatherTo{};
    float blend{};
    GAMEPLAY::_GET_WEATHER_TYPE_TRANSITION(
        &weatherFrom,
        &weatherTo,
        &blend);
    if (weatherFrom != 0U &&
        weatherTo != 0U &&
        std::isfinite(blend) &&
        blend >= 0.0F &&
        blend <= 1.0F) {
        state.flags =
            static_cast<std::uint8_t>(
                WorldStateFlag::WeatherValid);
        state.weatherFrom =
            static_cast<std::uint32_t>(weatherFrom);
        state.weatherTo =
            static_cast<std::uint32_t>(weatherTo);
        state.weatherBlend = blend;
    }
    return state;
#else
    return std::nullopt;
#endif
}

std::vector<HostWorldEntitySample>
ScriptHookSdkFacade::SampleWorldEntities(
    const float radiusMeters,
    const std::size_t maximumEntities) noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        if (!std::isfinite(radiusMeters) ||
            radiusMeters <= 0.0F ||
            maximumEntities == 0U) {
            return {};
        }

        const auto localPed = PLAYER::PLAYER_PED_ID();
        if (localPed == 0 ||
            ENTITY::DOES_ENTITY_EXIST(localPed) == FALSE) {
            return {};
        }
        const auto localPosition = ToBridgeVector(
            ENTITY::GET_ENTITY_COORDS(localPed, TRUE, FALSE));
        if (!IsFinite(localPosition)) {
            return {};
        }
        const auto remotePlayer =
            replicas_.FindLocal(remotePlayerId_);
        const bool missionActive =
            GAMEPLAY::GET_MISSION_FLAG() != FALSE;
        const bool cinematicPresentationActive =
            missionActive &&
            (CAM::IS_CINEMATIC_CAM_RENDERING() != FALSE ||
             (PLAYER::IS_PLAYER_CONTROL_ON(
                  PLAYER::PLAYER_ID()) == FALSE &&
              UI::IS_PAUSE_MENU_ACTIVE() == FALSE));

        std::array<int, kWorldPedPoolCapacity> peds{};
        const auto count = std::clamp(
            worldGetAllPeds(
                peds.data(),
                static_cast<int>(peds.size())),
            0,
            static_cast<int>(peds.size()));
        struct Candidate final {
            float distance{};
            HostWorldEntitySample sample{};
        };
        std::vector<Candidate> candidates;
        candidates.reserve(
            std::min<std::size_t>(
                static_cast<std::size_t>(count),
                maximumEntities * 2U));
        std::size_t rejectedInvalid{};
        std::size_t rejectedLocalOrPlayer{};
        std::size_t rejectedPlayerMount{};
        std::size_t rejectedProjectProxy{};
        std::size_t rejectedInvisible{};
        std::size_t rejectedPopulation{};
        std::size_t rejectedSpecies{};
        std::size_t rejectedRange{};
        std::size_t rejectedModelOrTransform{};
        std::size_t humanCandidates{};
        std::size_t nativeHumanCandidates{};
        std::size_t pedTypeHumanFallbackCandidates{};
        std::size_t horseCandidates{};
        std::size_t scriptOwnedCandidates{};
        std::size_t globalMissionCandidates{};
        std::size_t extendedMissionRangeCandidates{};
        std::size_t scenarioCandidates{};
        std::size_t blipCandidates{};
        std::size_t cinematicCandidates{};
        std::size_t combatCandidates{};
        std::size_t combatHostCandidates{};
        std::size_t combatGuestCandidates{};
        std::size_t shootingCandidates{};
        std::size_t fleeingHumanCandidates{};
        std::size_t fleeingHorseCandidates{};
        std::array<std::size_t, 9U> populationHistogram{};
        std::size_t pedTypeMale{};
        std::size_t pedTypeFemale{};
        std::size_t pedTypeAnimal{};
        std::size_t pedTypeOther{};

        for (int index = 0; index < count; ++index) {
            const auto ped = static_cast<Ped>(
                peds[static_cast<std::size_t>(index)]);
            if (ped == 0 ||
                ENTITY::DOES_ENTITY_EXIST(ped) == FALSE) {
                ++rejectedInvalid;
                continue;
            }
            if (ped == localPed ||
                (remotePlayer.has_value() &&
                 ped == *remotePlayer) ||
                PED::IS_PED_A_PLAYER(ped) != FALSE) {
                ++rejectedLocalOrPlayer;
                continue;
            }
            if (static_cast<LocalEntityHandle>(ped) ==
                localKnownMountHandle_) {
                // The active player horse has its own PlayerMountState lane;
                // mirroring it here would create a duplicate mount proxy.
                ++rejectedPlayerMount;
                continue;
            }
            if (replicas_.FindNetwork(ped).has_value() ||
                remoteMountReplicas_.FindNetwork(ped).has_value() ||
                worldEntityReplicas_.FindNetwork(ped).has_value()) {
                ++rejectedProjectProxy;
                continue;
            }
            const auto populationType =
                ENTITY::_GET_ENTITY_POPULATION_TYPE(ped);
            const auto populationBucket =
                populationType >= 0 && populationType <= 7
                    ? static_cast<std::size_t>(populationType)
                    : 8U;
            ++populationHistogram[populationBucket];
            // Script-owned shopkeepers, side interactions and mission actors
            // may exist while the coarse global mission flag is false. Treat
            // entity ownership itself as authoritative admission metadata.
            const bool scriptOwnedEntity =
                ENTITY::IS_ENTITY_A_MISSION_ENTITY(ped) != FALSE;
            const bool visibleEntity =
                ENTITY::IS_ENTITY_VISIBLE(ped) != FALSE &&
                ENTITY::GET_ENTITY_ALPHA(ped) > 0;
            if (!cinematicPresentationActive && !visibleEntity) {
                // RDR2 keeps cached process-owned horses and actors in the
                // ped pool with mission ownership after they have been hidden
                // locally. Mirroring those entries made them appear as random
                // horses on a newly joined guest. A live cinematic may hide
                // its cast transiently, so only apply this admission filter
                // outside cinematic presentation.
                ++rejectedInvisible;
                continue;
            }
            int pedType{};
            bool usedPedTypeFallback{};
            const bool reliableHuman = IsPedHumanReliable(
                ped,
                pedType,
                usedPedTypeFallback);
            pedTypeMale += pedType == 4 ? 1U : 0U;
            pedTypeFemale += pedType == 5 ? 1U : 0U;
            pedTypeAnimal += pedType == 28 ? 1U : 0U;
            pedTypeOther +=
                pedType != 4 && pedType != 5 && pedType != 28
                    ? 1U
                    : 0U;
            const bool horse =
                !reliableHuman &&
                CanPedBeMounted(ped) != FALSE;
            // Some scripted MetaPeds temporarily report a non-human species
            // while their mission outfit/graph is being assembled. They are
            // still mission actors and must not vanish from the host graph.
            const bool human =
                reliableHuman || (scriptOwnedEntity && !horse);
            const bool globalMissionEntity =
                missionActive && scriptOwnedEntity;
            if (!IsMirrorablePopulationType(populationType) &&
                !scriptOwnedEntity) {
                ++rejectedPopulation;
                continue;
            }
            if (!human && !horse) {
                ++rejectedSpecies;
                continue;
            }

            const auto position = ToBridgeVector(
                ENTITY::GET_ENTITY_COORDS(ped, TRUE, FALSE));
            const auto distance =
                Distance(localPosition, position);
            const auto admissionRadius =
                globalMissionEntity
                    ? std::max(
                          radiusMeters,
                          kMissionScriptOwnedActorRadiusMeters)
                    : radiusMeters;
            if (!IsFinite(position) ||
                !std::isfinite(distance) ||
                distance > admissionRadius) {
                ++rejectedRange;
                continue;
            }
            if (globalMissionEntity && distance > radiusMeters) {
                ++extendedMissionRangeCandidates;
            }

            const auto modelHash =
                static_cast<std::uint32_t>(
                    ENTITY::GET_ENTITY_MODEL(ped));
            if (modelHash == 0U) {
                ++rejectedModelOrTransform;
                continue;
            }

            std::uint8_t flags{};
            flags |= static_cast<std::uint8_t>(
                human
                    ? WorldEntityStateFlag::Human
                    : WorldEntityStateFlag::Horse);
            if (scriptOwnedEntity) {
                flags |= static_cast<std::uint8_t>(
                    WorldEntityStateFlag::ScriptOwned);
            }
            LocalEntityHandle parentLocalHandle{};
            if (human &&
                PED::IS_PED_ON_MOUNT(ped) != FALSE) {
                const auto mount = PED::GET_MOUNT(ped);
                if (mount != 0 &&
                    ENTITY::DOES_ENTITY_EXIST(mount) != FALSE) {
                    parentLocalHandle =
                        static_cast<LocalEntityHandle>(
                            mount);
                    flags |= static_cast<std::uint8_t>(
                        WorldEntityStateFlag::Mounted);
                }
            }

            const auto health =
                static_cast<float>(
                    ENTITY::GET_ENTITY_HEALTH(ped));
            const auto maximumHealth =
                static_cast<float>(
                    ENTITY::GET_ENTITY_MAX_HEALTH(
                        ped,
                        FALSE));
            const auto dead =
                health <= 0.0F ||
                PED::IS_PED_DEAD_OR_DYING(
                    ped,
                    TRUE) != FALSE;
            if (dead) {
                flags |= static_cast<std::uint8_t>(
                    WorldEntityStateFlag::Dead);
            }

            WorldCombatTargetSlot targetSlot{
                WorldCombatTargetSlot::None};
            if (remotePlayer.has_value() &&
                ENTITY::DOES_ENTITY_EXIST(*remotePlayer) != FALSE &&
                PED::IS_PED_IN_COMBAT(
                    ped,
                    static_cast<Ped>(*remotePlayer)) != FALSE) {
                targetSlot = WorldCombatTargetSlot::Guest;
            } else if (
                PED::IS_PED_IN_COMBAT(
                    ped,
                    localPed) != FALSE) {
                targetSlot = WorldCombatTargetSlot::Host;
            }
            if (targetSlot != WorldCombatTargetSlot::None) {
                flags |= static_cast<std::uint8_t>(
                    WorldEntityStateFlag::InCombat);
            }
            combatHostCandidates +=
                targetSlot == WorldCombatTargetSlot::Host ? 1U : 0U;
            combatGuestCandidates +=
                targetSlot == WorldCombatTargetSlot::Guest ? 1U : 0U;
            const bool usesScenario =
                PED::IS_PED_USING_ANY_SCENARIO(ped) != FALSE;
            const auto entityBlip = RADAR::GET_BLIP_FROM_ENTITY(ped);
            const bool hasEntityBlip =
                entityBlip != 0 &&
                RADAR::DOES_BLIP_EXIST(entityBlip) != FALSE;

            Hash weaponHash{};
            if (!human ||
                WEAPON::GET_CURRENT_PED_WEAPON(
                    ped,
                    &weaponHash,
                    TRUE,
                    0,
                    FALSE) == FALSE) {
                weaponHash = 0U;
            }
            if (human && weaponHash != 0U) {
                if (targetSlot !=
                        WorldCombatTargetSlot::None ||
                    PED::IS_PED_AIMING_FROM_COVER(
                        ped) != FALSE) {
                    flags |= static_cast<std::uint8_t>(
                        WorldEntityStateFlag::Aiming);
                }
                if (PED::IS_PED_SHOOTING(ped) != FALSE) {
                    flags |= static_cast<std::uint8_t>(
                        WorldEntityStateFlag::Firing);
                    flags |= static_cast<std::uint8_t>(
                        WorldEntityStateFlag::Aiming);
                    ++shootingCandidates;
                }
            }
            const auto humanFlag =
                static_cast<std::uint8_t>(
                    WorldEntityStateFlag::Human);
            const auto weaponActionFlags =
                static_cast<std::uint8_t>(
                    WorldEntityStateFlag::Aiming) |
                static_cast<std::uint8_t>(
                    WorldEntityStateFlag::Firing);
            if ((flags & humanFlag) == 0U) {
                // Animal weapon slots may report internal/unarmed hashes.
                // They are not portable weapon identities and would violate
                // the wire contract.
                weaponHash = 0U;
                flags &= ~weaponActionFlags;
            } else if (weaponHash == 0U) {
                flags &= ~weaponActionFlags;
            }
            auto heading = ENTITY::GET_ENTITY_HEADING(ped);
            if (!std::isfinite(heading)) {
                ++rejectedModelOrTransform;
                continue;
            }
            heading = std::fmod(heading, 360.0F);
            if (heading < 0.0F) {
                heading += 360.0F;
            }
            const auto healthFraction =
                maximumHealth > 0.0F
                    ? std::clamp(
                          health / maximumHealth,
                          0.0F,
                          1.0F)
                    : (dead ? 0.0F : 1.0F);
            const auto velocity = ToBridgeVector(
                ENTITY::GET_ENTITY_VELOCITY(
                    ped,
                    0));
            const bool fleeing =
                PED::IS_PED_FLEEING(ped) != FALSE;
            if (fleeing) {
                fleeingHumanCandidates += human ? 1U : 0U;
                fleeingHorseCandidates += horse ? 1U : 0U;
            }
            WorldTaskKind taskKind{
                WorldTaskKind::Idle};
            Vec3 taskTarget = position;
            if (dead) {
                taskKind = WorldTaskKind::Dead;
            } else if (parentLocalHandle != 0) {
                taskKind = WorldTaskKind::Mounted;
            } else if (
                cinematicPresentationActive &&
                scriptOwnedEntity) {
                // A Story AnimScene owns the actor graph. Do not replace it
                // with TASK_STAND_STILL on the guest; the cinematic lane
                // carries the authoritative root at camera frequency while
                // leaving the local visual graph free for future clip data.
                taskKind = WorldTaskKind::Cinematic;
            } else if (
                targetSlot !=
                    WorldCombatTargetSlot::None) {
                taskKind = WorldTaskKind::Combat;
                const auto targetPed =
                    targetSlot ==
                            WorldCombatTargetSlot::Host
                        ? localPed
                        : remotePlayer.value_or(0);
                if (targetPed != 0 &&
                    ENTITY::DOES_ENTITY_EXIST(
                        targetPed) != FALSE) {
                    taskTarget = ToBridgeVector(
                        ENTITY::GET_ENTITY_COORDS(
                            targetPed,
                            TRUE,
                            FALSE));
                }
            } else if (fleeing) {
                taskKind = WorldTaskKind::Fleeing;
            } else if (hasEntityBlip || usesScenario) {
                taskKind = WorldTaskKind::Scenario;
            } else {
                const auto speedSquared =
                    velocity.x * velocity.x +
                    velocity.y * velocity.y +
                    velocity.z * velocity.z;
                if (std::isfinite(speedSquared) &&
                    speedSquared > 0.04F) {
                    taskKind =
                        WorldTaskKind::Locomotion;
                }
            }
            if (taskKind == WorldTaskKind::Locomotion ||
                taskKind == WorldTaskKind::Fleeing) {
                taskTarget = {
                    position.x + velocity.x * 1.5F,
                    position.y + velocity.y * 1.5F,
                    position.z + velocity.z * 1.5F};
            }

            auto selectionPriority =
                HostWorldEntityPriority::Ambient;
            if (scriptOwnedEntity) {
                selectionPriority =
                    HostWorldEntityPriority::ScriptOwned;
            } else if (
                targetSlot != WorldCombatTargetSlot::None) {
                selectionPriority =
                    HostWorldEntityPriority::Combat;
            } else if (usesScenario) {
                // Scenarios cover generic service/interaction actors without
                // hard-coding mission or shopkeeper model hashes.
                selectionPriority =
                    HostWorldEntityPriority::Interactive;
            } else if (parentLocalHandle != 0) {
                selectionPriority =
                    HostWorldEntityPriority::Scenario;
            }

            candidates.push_back(
                Candidate{
                    distance,
                    HostWorldEntitySample{
                        static_cast<LocalEntityHandle>(ped),
                        modelHash,
                        WorldEntityKind::Ped,
                        flags,
                        targetSlot,
                        position,
                        velocity,
                        heading,
                        healthFraction,
                        static_cast<std::uint32_t>(
                        weaponHash),
                        taskKind,
                        parentLocalHandle,
                        taskTarget,
                        selectionPriority,
                        distance}});
            humanCandidates += human ? 1U : 0U;
            nativeHumanCandidates +=
                reliableHuman && !usedPedTypeFallback ? 1U : 0U;
            pedTypeHumanFallbackCandidates +=
                usedPedTypeFallback ? 1U : 0U;
            horseCandidates += horse ? 1U : 0U;
            scriptOwnedCandidates += scriptOwnedEntity ? 1U : 0U;
            globalMissionCandidates += globalMissionEntity ? 1U : 0U;
            scenarioCandidates += usesScenario ? 1U : 0U;
            blipCandidates += hasEntityBlip ? 1U : 0U;
            cinematicCandidates +=
                taskKind == WorldTaskKind::Cinematic ? 1U : 0U;
            combatCandidates +=
                targetSlot != WorldCombatTargetSlot::None ? 1U : 0U;
        }

        // AnimScenes frequently bind chairs, weapons, bottles and cigarettes
        // as required named roles. The generic world mirror used to carry
        // only peds, so those roles had no NetEntityId and the guest tried to
        // load an incomplete definition forever. Mirror only object handles
        // observed in the currently started captured Story scene; this keeps
        // the normal world bubble bounded and avoids replicating ambient
        // debris.
        std::size_t animSceneObjectCandidates{};
        for (const auto localHandle :
             g_storyVmAnimSceneCapture.ActiveRoleEntityHandles()) {
            const auto object = static_cast<Entity>(localHandle);
            if (object == 0 ||
                ENTITY::DOES_ENTITY_EXIST(object) == FALSE ||
                ENTITY::GET_ENTITY_TYPE(object) != 3 ||
                replicas_.FindNetwork(localHandle).has_value() ||
                remoteMountReplicas_.FindNetwork(localHandle).has_value() ||
                worldEntityReplicas_.FindNetwork(localHandle).has_value()) {
                continue;
            }
            const auto position = ToBridgeVector(
                ENTITY::GET_ENTITY_COORDS(object, TRUE, FALSE));
            const auto distance = Distance(localPosition, position);
            const auto modelHash = static_cast<std::uint32_t>(
                ENTITY::GET_ENTITY_MODEL(object));
            auto heading = ENTITY::GET_ENTITY_HEADING(object);
            const auto velocity = ToBridgeVector(
                ENTITY::GET_ENTITY_VELOCITY(object, 0));
            if (!IsFinite(position) || !IsFinite(velocity) ||
                !std::isfinite(distance) ||
                distance > kMissionScriptOwnedActorRadiusMeters ||
                modelHash == 0U || !std::isfinite(heading)) {
                continue;
            }
            heading = std::fmod(heading, 360.0F);
            if (heading < 0.0F) {
                heading += 360.0F;
            }
            candidates.push_back(Candidate{
                distance,
                HostWorldEntitySample{
                    localHandle,
                    modelHash,
                    WorldEntityKind::Object,
                    static_cast<std::uint8_t>(
                        WorldEntityStateFlag::ScriptOwned),
                    WorldCombatTargetSlot::None,
                    position,
                    velocity,
                    heading,
                    1.0F,
                    0U,
                    WorldTaskKind::Cinematic,
                    0,
                    position,
                    HostWorldEntityPriority::ScriptOwned,
                    distance}});
            ++animSceneObjectCandidates;
        }

        std::ranges::sort(
            candidates,
            [](const Candidate& lhs, const Candidate& rhs) {
                if (lhs.sample.selectionPriority !=
                    rhs.sample.selectionPriority) {
                    return lhs.sample.selectionPriority >
                           rhs.sample.selectionPriority;
                }
                if (lhs.distance != rhs.distance) {
                    return lhs.distance < rhs.distance;
                }
                return lhs.sample.localHandle <
                       rhs.sample.localHandle;
            });
        std::vector<HostWorldEntitySample> result;
        result.reserve(
            std::min(maximumEntities, candidates.size()));
        std::unordered_map<
            LocalEntityHandle,
            const Candidate*> candidatesByHandle;
        candidatesByHandle.reserve(candidates.size());
        for (const auto& candidate : candidates) {
            candidatesByHandle.emplace(
                candidate.sample.localHandle,
                &candidate);
        }
        std::unordered_set<LocalEntityHandle> selected;
        selected.reserve(maximumEntities);
        std::size_t dependencyPairsDeferred{};
        for (const auto& candidate : candidates) {
            if (result.size() >= maximumEntities ||
                selected.contains(candidate.sample.localHandle)) {
                continue;
            }
            const auto parentHandle =
                candidate.sample.parentLocalHandle;
            if (parentHandle != 0 &&
                !selected.contains(parentHandle)) {
                const auto parent = candidatesByHandle.find(
                    parentHandle);
                if (parent == candidatesByHandle.end() ||
                    result.size() + 2U > maximumEntities) {
                    ++dependencyPairsDeferred;
                    continue;
                }
                result.push_back(parent->second->sample);
                selected.insert(parentHandle);
            }
            if (result.size() >= maximumEntities) {
                ++dependencyPairsDeferred;
                continue;
            }
            result.push_back(candidate.sample);
            selected.insert(candidate.sample.localHandle);
        }
        const auto resultSize = result.size();
        const auto now = TickMilliseconds();
        if (previousWorldSampleDiagnosticsMs_ == 0U ||
            now < previousWorldSampleDiagnosticsMs_ ||
            now - previousWorldSampleDiagnosticsMs_ >=
                kWorldMirrorDiagnosticsMilliseconds) {
            previousWorldSampleDiagnosticsMs_ = now;
            Log(
                "[WORLD_POOL][ENTITY_GRAPH_SAMPLE] v20 pool=" +
                std::to_string(count) +
                ", candidates=" +
                std::to_string(candidates.size()) +
                ", human=" +
                std::to_string(humanCandidates) +
                ", human-native=" +
                std::to_string(nativeHumanCandidates) +
                ", human-pedtype-fallback=" +
                std::to_string(pedTypeHumanFallbackCandidates) +
                ", horse=" +
                std::to_string(horseCandidates) +
                ", script-owned=" +
                std::to_string(scriptOwnedCandidates) +
                ", global-mission-script-owned=" +
                std::to_string(globalMissionCandidates) +
                ", extended-mission-range=" +
                std::to_string(extendedMissionRangeCandidates) +
                ", scenario=" +
                std::to_string(scenarioCandidates) +
                ", entity-blip=" +
                std::to_string(blipCandidates) +
                ", cinematic-root-lane=" +
                std::to_string(cinematicCandidates) +
                ", animscene-objects=" +
                std::to_string(animSceneObjectCandidates) +
                ", combat=" +
                std::to_string(combatCandidates) +
                ", combat-host=" +
                std::to_string(combatHostCandidates) +
                ", combat-guest=" +
                std::to_string(combatGuestCandidates) +
                ", shooting=" +
                std::to_string(shootingCandidates) +
                ", fleeing-human=" +
                std::to_string(fleeingHumanCandidates) +
                ", fleeing-horse=" +
                std::to_string(fleeingHorseCandidates) +
                ", emitted=" +
                std::to_string(resultSize) +
                ", dependency-deferred=" +
                std::to_string(dependencyPairsDeferred) +
                ", reject-invalid=" +
                std::to_string(rejectedInvalid) +
                ", reject-player=" +
                std::to_string(rejectedLocalOrPlayer) +
                ", reject-player-mount=" +
                std::to_string(rejectedPlayerMount) +
                ", reject-proxy=" +
                std::to_string(rejectedProjectProxy) +
                ", reject-invisible=" +
                std::to_string(rejectedInvisible) +
                ", reject-population=" +
                std::to_string(rejectedPopulation) +
                ", reject-species=" +
                std::to_string(rejectedSpecies) +
                ", reject-range=" +
                std::to_string(rejectedRange) +
                ", reject-model-transform=" +
                std::to_string(rejectedModelOrTransform) +
                ", population-types=0:" +
                std::to_string(populationHistogram[0]) +
                ",1:" +
                std::to_string(populationHistogram[1]) +
                ",2:" +
                std::to_string(populationHistogram[2]) +
                ",3:" +
                std::to_string(populationHistogram[3]) +
                ",4:" +
                std::to_string(populationHistogram[4]) +
                ",5:" +
                std::to_string(populationHistogram[5]) +
                ",6:" +
                std::to_string(populationHistogram[6]) +
                ",7:" +
                std::to_string(populationHistogram[7]) +
                ",other:" +
                std::to_string(populationHistogram[8]) +
                ", ped-types=male4:" +
                std::to_string(pedTypeMale) +
                ",female5:" +
                std::to_string(pedTypeFemale) +
                ",animal28:" +
                std::to_string(pedTypeAnimal) +
                ",other:" +
                std::to_string(pedTypeOther));
        }
        return result;
#else
        (void)radiusMeters;
        (void)maximumEntities;
#endif
    } catch (...) {
        // Pool enumeration is optional and must never destabilize Story Mode.
    }
    return {};
}

std::optional<DamageIntentPayload>
ScriptHookSdkFacade::SampleWorldDamageIntent(
    const NetEntityId attackerId) noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        if (!attackerId.IsValid()) {
            return std::nullopt;
        }
        const auto localPed = PLAYER::PLAYER_PED_ID();
        if (localPed == 0 ||
            ENTITY::DOES_ENTITY_EXIST(localPed) == FALSE ||
            PED::IS_PED_SHOOTING(localPed) == FALSE) {
            return std::nullopt;
        }

        const auto now = TickMilliseconds();
        if (previousWorldDamageIntentMs_ != 0U &&
            now >= previousWorldDamageIntentMs_ &&
            now - previousWorldDamageIntentMs_ <
                kWorldDamageIntentMinimumIntervalMilliseconds) {
            return std::nullopt;
        }

        Entity target{};
        if (PLAYER::GET_ENTITY_PLAYER_IS_FREE_AIMING_AT(
                PLAYER::PLAYER_ID(),
                &target) == FALSE ||
            target == 0) {
            return std::nullopt;
        }
        const auto targetId =
            worldEntityReplicas_.FindNetwork(
                static_cast<LocalEntityHandle>(target));
        if (!targetId.has_value()) {
            return std::nullopt;
        }

        Hash weaponHash{};
        if (WEAPON::GET_CURRENT_PED_WEAPON(
                localPed,
                &weaponHash,
                TRUE,
                0,
                FALSE) == FALSE ||
            weaponHash == 0U ||
            WEAPON::IS_WEAPON_VALID(weaponHash) == FALSE) {
            return std::nullopt;
        }

        ++worldDamageShotSequence_;
        if (worldDamageShotSequence_ == 0U) {
            worldDamageShotSequence_ = 1U;
        }
        previousWorldDamageIntentMs_ = now;
        return DamageIntentPayload{
            attackerId,
            *targetId,
            static_cast<std::uint32_t>(weaponHash),
            kWorldDamageIntentFixedDamage,
            worldDamageShotSequence_};
#else
        (void)attackerId;
#endif
    } catch (...) {
        // Shooting an optional proxy must not escape the game script tick.
    }
    return std::nullopt;
}

std::optional<float>
ScriptHookSdkFacade::HostGuestDistanceMeters() noexcept {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
    const auto remote = replicas_.FindLocal(remotePlayerId_);
    const auto local = PLAYER::PLAYER_PED_ID();
    if (!remote.has_value() || local == 0 ||
        ENTITY::DOES_ENTITY_EXIST(local) == FALSE ||
        ENTITY::DOES_ENTITY_EXIST(*remote) == FALSE) {
        return std::nullopt;
    }
    const auto localPosition = ToBridgeVector(
        ENTITY::GET_ENTITY_COORDS(local, TRUE, FALSE));
    const auto remotePosition = ToBridgeVector(
        ENTITY::GET_ENTITY_COORDS(*remote, TRUE, FALSE));
    return Distance(localPosition, remotePosition);
#endif
    return std::nullopt;
}

MenuInputState ScriptHookSdkFacade::ReadMenuInput() noexcept {
    return {
        .f7 = Pressed(VK_F7),
        .f8 = Pressed(VK_F8),
        .f9 = Pressed(VK_F9),
        .f10 = Pressed(VK_F10),
        .up = Pressed(VK_UP),
        .down = Pressed(VK_DOWN),
        .left = Pressed(VK_LEFT),
        .right = Pressed(VK_RIGHT),
        // Some virtual-key input sources (including accessibility remoting)
        // do not surface their Return event as VK_RETURN to GetAsyncKeyState.
        // Space is the conventional alternate menu-confirm key and gives the
        // emergency/test panel an equivalent, reliable activation path.
        .confirm = Pressed(VK_RETURN) || Pressed(VK_SPACE),
        .cancel = Pressed(VK_ESCAPE)};
}

void ScriptHookSdkFacade::DrawMenu(
    const bool open,
    const std::span<const BridgeCommand> commands,
    const std::size_t selected) noexcept {
    try {
        const bool changed =
            open != previousMenuOpen_ ||
            (open && selected != previousSelection_);
        previousMenuOpen_ = open;
        previousSelection_ = selected;
        if (changed) {
            if (!open) {
                Log("F9 menu closed");
            } else if (selected < commands.size()) {
                std::string text{"F9 menu: "};
                text.append(MenuController::Label(commands[selected]));
                Log(text);
            }
        }

#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        if (!open) {
            return;
        }

        constexpr float kLeft = 0.035F;
        constexpr float kTop = 0.105F;
        constexpr float kWidth = 0.66F;
        constexpr float kHeaderHeight = 0.066F;
        constexpr float kSectionHeight = 0.034F;
        constexpr float kRowHeight = 0.040F;
        constexpr float kFooterHeight = 0.085F;
        constexpr float kGutter = 0.018F;
        constexpr auto kColumnSize =
            MenuController::PrimaryCommandCount();
        const auto columnWidth = (kWidth - kGutter) * 0.5F;
        const auto menuHeight = kHeaderHeight + kSectionHeight +
            (static_cast<float>(kColumnSize) * kRowHeight) +
            kFooterHeight;
        DrawNativeRectangle(
            kLeft + (kWidth * 0.5F),
            kTop + (menuHeight * 0.5F),
            kWidth,
            menuHeight,
            12,
            7,
            7,
            238);
        DrawNativeRectangle(
            kLeft + (kWidth * 0.5F),
            kTop + (kHeaderHeight * 0.5F),
            kWidth,
            kHeaderHeight,
            105,
            20,
            17,
            245);
        DrawNativeRectangle(
            kLeft + 0.004F,
            kTop + (kHeaderHeight * 0.5F),
            0.008F,
            kHeaderHeight,
            224,
            169,
            75,
            255);
        DrawNativeText(
            "COOP STORY - SESSION MENU",
            kLeft + 0.020F,
            kTop + 0.013F,
            0.325F,
            255,
            240,
            214);
        DrawNativeText(
            "Common actions on the left, test tools on the right",
            kLeft + 0.020F,
            kTop + 0.040F,
            0.195F,
            220,
            196,
            168);

        const auto rowsTop = kTop + kHeaderHeight + kSectionHeight;
        DrawNativeText(
            "SESSION AND RECOVERY",
            kLeft + 0.014F,
            kTop + kHeaderHeight + 0.008F,
            0.215F,
            224,
            169,
            75);
        DrawNativeText(
            "TEST TOOLS",
            kLeft + columnWidth + kGutter + 0.014F,
            kTop + kHeaderHeight + 0.008F,
            0.215F,
            132,
            173,
            205);

        for (std::size_t index = 0U;
             index < commands.size();
             ++index) {
            const auto column = index / kColumnSize;
            const auto row = index % kColumnSize;
            const auto columnLeft = kLeft +
                (static_cast<float>(column) *
                 (columnWidth + kGutter));
            const auto rowTop = rowsTop +
                (static_cast<float>(row) * kRowHeight);
            const bool isSelected = index == selected;
            if (isSelected) {
                DrawNativeRectangle(
                    columnLeft + (columnWidth * 0.5F),
                    rowTop + (kRowHeight * 0.5F),
                    columnWidth,
                    kRowHeight,
                    column == 0U ? 128 : 38,
                    column == 0U ? 35 : 82,
                    column == 0U ? 29 : 108,
                    230);
                DrawNativeRectangle(
                    columnLeft + 0.003F,
                    rowTop + (kRowHeight * 0.5F),
                    0.006F,
                    kRowHeight,
                    column == 0U ? 224 : 132,
                    column == 0U ? 169 : 173,
                    column == 0U ? 75 : 205,
                    255);
            }
            std::string label{
                isSelected ? "> " : "  "};
            label.append(MenuController::Label(commands[index]));
            DrawNativeText(
                label,
                columnLeft + 0.014F,
                rowTop + 0.008F,
                0.235F,
                isSelected ? 255 : 215,
                isSelected ? 240 : 218,
                isSelected ? 214 : 205);
        }

        const auto footerY = rowsTop +
            (static_cast<float>(kColumnSize) * kRowHeight);
        DrawNativeRectangle(
            kLeft + (kWidth * 0.5F),
            footerY + (kFooterHeight * 0.5F),
            kWidth,
            kFooterHeight,
            25,
            14,
            13,
            242);
        DrawNativeText(
            "ARROWS: select / group   ENTER: run   F9 or ESC: close",
            kLeft + 0.014F,
            footerY + 0.014F,
            0.205F,
            200,
            188,
            174);
        DrawNativeText(
            "F7: save an error marker without opening the menu",
            kLeft + 0.014F,
            footerY + 0.047F,
            0.215F,
            232,
            176,
            92);
#endif
    } catch (...) {
        // HUD rendering must never take down the game script thread.
    }
}

void ScriptHookSdkFacade::DrawSessionMenu(
    const SessionOverlayView& state) noexcept {
    try {
        const bool changed =
            state.open != previousSessionMenuOpen_ ||
            state.phase != previousSessionPhase_ ||
            (state.open &&
             state.selection != previousSessionSelection_);
        previousSessionMenuOpen_ = state.open;
        previousSessionPhase_ = state.phase;
        previousSessionSelection_ = state.selection;
        if (changed) {
            Log(
                state.open
                    ? "session overlay visible"
                    : "session overlay hidden");
        }

#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        if (!state.open) {
            return;
        }

        constexpr float kLeft = 0.27F;
        constexpr float kTop = 0.19F;
        constexpr float kWidth = 0.46F;
        constexpr float kHeaderHeight = 0.068F;
        constexpr float kRowHeight = 0.047F;
        constexpr float kFooterHeight = 0.105F;
        const auto menuHeight =
            kHeaderHeight +
            static_cast<float>(state.actions.size()) * kRowHeight +
            kFooterHeight;
        DrawNativeRectangle(
            kLeft + kWidth * 0.5F,
            kTop + menuHeight * 0.5F,
            kWidth,
            menuHeight,
            5,
            9,
            12,
            230);
        DrawNativeRectangle(
            kLeft + kWidth * 0.5F,
            kTop + kHeaderHeight * 0.5F,
            kWidth,
            kHeaderHeight,
            90,
            32,
            22,
            240);
        DrawNativeText(
            "COOP STORY - EMERGENCY PANEL (F8)",
            kLeft + 0.018F,
            kTop + 0.018F,
            0.32F,
            255,
            240,
            220);

        for (std::size_t index = 0U;
             index < state.actions.size();
             ++index) {
            const auto rowTop =
                kTop + kHeaderHeight +
                static_cast<float>(index) * kRowHeight;
            const bool selected = index == state.selection;
            if (selected) {
                DrawNativeRectangle(
                    kLeft + kWidth * 0.5F,
                    rowTop + kRowHeight * 0.5F,
                    kWidth,
                    kRowHeight,
                    36,
                    116,
                    125,
                    220);
            }
            std::string label{selected ? "> " : "  "};
            label.append(
                SessionMenuController::Label(state.actions[index]));
            DrawNativeText(
                label,
                kLeft + 0.018F,
                rowTop + 0.010F,
                0.265F,
                selected ? 255 : 215,
                selected ? 255 : 225,
                selected ? 255 : 225);
        }

        const auto footerY =
            kTop + kHeaderHeight +
            static_cast<float>(state.actions.size()) * kRowHeight;
        DrawNativeText(
            state.status,
            kLeft + 0.018F,
            footerY + 0.013F,
            0.225F,
            state.phase == SessionOverlayPhase::Error ? 255 : 175,
            state.phase == SessionOverlayPhase::Error ? 135 : 220,
            state.phase == SessionOverlayPhase::Error ? 110 : 205);
        DrawNativeText(
            "HOST/JOIN: launcher | F8 panel | F10 bar | ESC close",
            kLeft + 0.018F,
            footerY + 0.058F,
            0.205F,
            155,
            190,
            195);
#endif
    } catch (...) {
        // The overlay is best effort and must not take down the script thread.
    }
}

void ScriptHookSdkFacade::DrawNotification(
    const std::string_view text,
    const bool success) noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        if (text.empty()) {
            return;
        }
        constexpr float kWidth = 0.31F;
        constexpr float kHeight = 0.067F;
        constexpr float kRight = 0.975F;
        constexpr float kBottom = 0.925F;
        const auto left = kRight - kWidth;
        const auto top = kBottom - kHeight;
        DrawNativeRectangle(
            left + (kWidth * 0.5F),
            top + (kHeight * 0.5F),
            kWidth,
            kHeight,
            16,
            10,
            9,
            235);
        DrawNativeRectangle(
            left + 0.004F,
            top + (kHeight * 0.5F),
            0.008F,
            kHeight,
            success ? 108 : 205,
            success ? 184 : 72,
            success ? 126 : 55,
            255);
        DrawNativeText(
            success ? "DIAGNOSTICS SAVED" : "WARNING",
            left + 0.018F,
            top + 0.010F,
            0.195F,
            success ? 130 : 232,
            success ? 214 : 176,
            success ? 151 : 92);
        DrawNativeText(
            text,
            left + 0.018F,
            top + 0.033F,
            0.245F,
            246,
            239,
            225);
#else
        (void)text;
        (void)success;
#endif
    } catch (...) {
        // A transient QoL toast must never interrupt the script thread.
    }
}

std::string ScriptHookSdkFacade::ReadClipboardText() noexcept {
    try {
        if (::OpenClipboard(nullptr) == FALSE) {
            return {};
        }
        const auto handle = ::GetClipboardData(CF_UNICODETEXT);
        if (handle == nullptr) {
            ::CloseClipboard();
            return {};
        }
        const auto* text =
            static_cast<const wchar_t*>(::GlobalLock(handle));
        if (text == nullptr) {
            ::CloseClipboard();
            return {};
        }
        constexpr std::size_t kMaximumClipboardCharacters = 2'048U;
        const auto bounded = ::wcsnlen_s(
            text,
            kMaximumClipboardCharacters + 1U);
        if (bounded > kMaximumClipboardCharacters) {
            ::GlobalUnlock(handle);
            ::CloseClipboard();
            return {};
        }
        auto result = Utf8FromWide(
            std::wstring_view{text, bounded});
        ::GlobalUnlock(handle);
        ::CloseClipboard();
        return result;
    } catch (...) {
        ::CloseClipboard();
        return {};
    }
}

bool ScriptHookSdkFacade::WriteClipboardText(
    const std::string_view text) noexcept {
    try {
        const auto wide = WideFromUtf8(text);
        if (wide.empty()) {
            return false;
        }
        const auto bytes =
            (wide.size() + 1U) * sizeof(wchar_t);
        const auto memory =
            ::GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
        if (memory == nullptr) {
            return false;
        }
        auto* destination =
            static_cast<wchar_t*>(::GlobalLock(memory));
        if (destination == nullptr) {
            ::GlobalFree(memory);
            return false;
        }
        std::copy(wide.begin(), wide.end(), destination);
        destination[wide.size()] = L'\0';
        ::GlobalUnlock(memory);

        if (::OpenClipboard(nullptr) == FALSE) {
            ::GlobalFree(memory);
            return false;
        }
        if (::EmptyClipboard() == FALSE ||
            ::SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
            ::CloseClipboard();
            ::GlobalFree(memory);
            return false;
        }
        ::CloseClipboard();
        return true;
    } catch (...) {
        ::CloseClipboard();
        return false;
    }
}

void ScriptHookSdkFacade::DrawBridgeHud(
    const BridgeHudState& state) noexcept {
    try {
        const bool changed =
            !hasPreviousHudState_ ||
            state.bridgeActive != previousHudState_.bridgeActive ||
            state.sidecarConnected != previousHudState_.sidecarConnected ||
            state.localSlot != previousHudState_.localSlot ||
            state.remoteConnected != previousHudState_.remoteConnected ||
            state.diagnosticsEnabled !=
                previousHudState_.diagnosticsEnabled ||
            state.soloOverrideEnabled !=
                previousHudState_.soloOverrideEnabled ||
            state.reviveAvailable != previousHudState_.reviveAvailable ||
            state.missionConflict != previousHudState_.missionConflict;
        previousHudState_ = state;
        hasPreviousHudState_ = true;

        std::string role{"COOP: WAITING"};
        if (state.localSlot.has_value()) {
            role =
                *state.localSlot == PlayerSlot::Host
                    ? "COOP HOST"
                    : "COOP GUEST";
        }
        const std::string ipc =
            state.sidecarConnected
                ? "IPC CONNECTED"
                : "IPC DISCONNECTED";
        const std::string remote =
            state.remoteConnected
                ? "REMOTE STREAMING"
                : "REMOTE NONE";
        int red{};
        int green{};
        int blue{};
        if (!state.sidecarConnected) {
            red = 255;
            green = 145;
            blue = 90;
        } else if (!state.localSlot.has_value()) {
            red = 255;
            green = 210;
            blue = 90;
        } else if (!state.remoteConnected) {
            red = 255;
            green = 210;
            blue = 90;
        } else {
            red = 95;
            green = 235;
            blue = 165;
        }

        if (changed) {
            Log(
                "HUD state: " + role + ", " + ipc + ", " + remote);
        }

#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        std::string line{role};
        line.append(" | ");
        line.append(ipc);
        line.append(" | ");
        line.append(remote);
        line.append(" | F8 MENU | F9 | F10 HIDE");
        if (state.soloOverrideEnabled) {
            line.append(" | SOLO");
        }
        if (state.diagnosticsEnabled) {
            line.append(" | DIAG");
        }
        DrawNativeRectangle(
            0.25F,
            0.028F,
            0.48F,
            0.036F,
            4,
            9,
            12,
            190);
        DrawNativeText(
            line,
            0.016F,
            0.016F,
            0.235F,
            red,
            green,
            blue);
        if (state.reviveAvailable) {
            const auto percent = static_cast<int>(std::lround(
                std::clamp(state.reviveProgress, 0.0F, 1.0F) * 100.0F));
            const std::string prompt = state.reviveProgress > 0.0F
                ? "REVIVING PEER " + std::to_string(percent) + "% — KEEP HOLDING CONTEXT"
                : "HOLD CONTEXT NEAR YOUR DOWNED PEER TO REVIVE";
            DrawNativeRectangle(
                0.5F,
                0.82F,
                0.54F,
                0.042F,
                4,
                9,
                12,
                210);
            DrawNativeText(
                prompt,
                0.016F,
                0.81F,
                0.265F,
                255,
                235,
                170);
        }
        if (state.missionConflict) {
            DrawNativeRectangle(
                0.5F,
                0.765F,
                0.68F,
                0.042F,
                55,
                18,
                12,
                220);
            DrawNativeText(
                "LOCAL STORY MISSION IS ISOLATED — EXIT IT TO FOLLOW THE HOST",
                0.016F,
                0.755F,
                0.225F,
                255,
                205,
                130);
        }
#endif
    } catch (...) {
        // HUD rendering and status logging are best effort only.
    }
}

void ScriptHookSdkFacade::DrawPauseVoteStatus(
    const PauseVoteView& state) noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        if (!state.sessionActive ||
            state.paused ||
            (!state.hostVoted &&
             !state.guestVoted)) {
            return;
        }

        std::string text;
        if (state.hostVoted) {
            text =
                "HOST requests a pause | ESC: confirm";
        } else {
            text =
                "GUEST requests a pause | ESC: confirm";
        }
        text.append(
            state.hostVoted
                ? " | HOST OK"
                : " | HOST ...");
        text.append(
            state.guestVoted
                ? " | GUEST OK"
                : " | GUEST ...");
        DrawNativeRectangle(
            0.5F,
            0.105F,
            0.58F,
            0.052F,
            55,
            42,
            18,
            220);
        DrawNativeText(
            text,
            0.222F,
            0.090F,
            0.285F,
            245,
            225,
            165,
            255,
            false);
#else
        (void)state;
#endif
    } catch (...) {
        // The synchronized pause remains usable through raw Escape even if
        // the optional status overlay cannot be rendered.
    }
}

void ScriptHookSdkFacade::ShowMissionBubbleWarning(
    const float distanceMeters) noexcept {
    try {
        const auto now = TickMilliseconds();
        if (now < previousBubbleLogMs_ ||
            now - previousBubbleLogMs_ >= 1'000U) {
            Log(
                "mission bubble warning: " +
                std::to_string(distanceMeters) +
                " m");
            previousBubbleLogMs_ = now;
        }
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        DrawNativeRectangle(
            0.5F,
            0.095F,
            0.42F,
            0.042F,
            85,
            34,
            20,
            205);
        DrawNativeText(
            "COOP: move closer to the host (mission limit)",
            0.305F,
            0.082F,
            0.27F,
            255,
            205,
            120);
#endif
    } catch (...) {
        // Warning rendering is best effort.
    }
}

bool ScriptHookSdkFacade::ExecuteCommand(
    const BridgeCommand command) noexcept {
    // No memory writes or pattern scans are allowed. Game actions remain
    // disabled until their public native calls are validated for 1491.50.
    switch (command) {
        case BridgeCommand::GrantTestPistol: {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
            try {
                const auto ped = PLAYER::PLAYER_PED_ID();
                if (ped == 0 ||
                    ENTITY::DOES_ENTITY_EXIST(ped) == FALSE) {
                    return false;
                }
                char weaponName[] = "WEAPON_REVOLVER_CATTLEMAN";
                const auto weapon =
                    GAMEPLAY::GET_HASH_KEY(weaponName);
                if (weapon == 0U ||
                    WEAPON::IS_WEAPON_VALID(weapon) == FALSE) {
                    return false;
                }
                int maximumAmmo = 999;
                int reportedMaximum{};
                if (WEAPON::GET_MAX_AMMO(
                        ped,
                        &reportedMaximum,
                        weapon) != FALSE &&
                    reportedMaximum > 0) {
                    maximumAmmo = reportedMaximum;
                }
                WEAPON::GIVE_DELAYED_WEAPON_TO_PED(
                    ped,
                    weapon,
                    maximumAmmo,
                    TRUE,
                    0);
                WEAPON::SET_PED_AMMO(
                    ped,
                    weapon,
                    maximumAmmo);
                WEAPON::SET_CURRENT_PED_WEAPON(
                    ped,
                    weapon,
                    TRUE,
                    0,
                    FALSE,
                    FALSE);
                Log(
                    "[INFO][TEST_WEAPON] Cattleman + max ammo przyznany lokalnie");
                return true;
            } catch (...) {
                return false;
            }
#else
            return false;
#endif
        }
        case BridgeCommand::GrantTestLasso: {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
            try {
                const auto ped = PLAYER::PLAYER_PED_ID();
                if (ped == 0 ||
                    ENTITY::DOES_ENTITY_EXIST(ped) == FALSE) {
                    Log(
                        "[ERROR][TEST_WEAPON] local player not found; lasso was not granted");
                    return false;
                }
                const auto lasso =
                    static_cast<Hash>(kWeaponLasso);
                // IS_WEAPON_VALID is not a reliable gate for utility weapons
                // in the prologue. Force the delayed grant and equip it now;
                // this is the same path used by the working remote lasso actor.
                WEAPON::GIVE_DELAYED_WEAPON_TO_PED(
                    ped,
                    lasso,
                    1,
                    TRUE,
                    0);
                WEAPON::SET_PED_AMMO(ped, lasso, 1);
                WEAPON::SET_CURRENT_PED_WEAPON(
                    ped,
                    lasso,
                    TRUE,
                    0,
                    FALSE,
                    FALSE);
                const bool owned =
                    WEAPON::HAS_PED_GOT_WEAPON(
                        ped,
                        lasso,
                        FALSE,
                        FALSE) != FALSE;
                Hash selectedWeapon{};
                const bool selected =
                    WEAPON::GET_CURRENT_PED_WEAPON(
                        ped,
                        &selectedWeapon,
                        TRUE,
                        0,
                        FALSE) != FALSE &&
                    selectedWeapon == lasso;
                Log(
                    "[INFO][TEST_WEAPON] wymuszono lasso lokalnie; owned=" +
                    std::to_string(owned ? 1 : 0) +
                    ", selected=" +
                    std::to_string(selected ? 1 : 0) +
                    ", hash=" +
                    std::to_string(kWeaponLasso));
                return true;
            } catch (...) {
                Log(
                    "[ERROR][TEST_WEAPON] exception while granting the lasso");
                return false;
            }
#else
            return false;
#endif
        }
        case BridgeCommand::ProbeRepeatingShotgunShopUnlock:
        case BridgeCommand::EnableRepeatingShotgunShopUnlock: {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
            try {
                char weaponName[] = "WEAPON_SHOTGUN_REPEATING";
                const auto weapon = GAMEPLAY::GET_HASH_KEY(weaponName);
                if (weapon == 0U || WEAPON::IS_WEAPON_VALID(weapon) == FALSE) {
                    Log("[ERROR][SHOP_UNLOCK] Repeating Shotgun is not a valid weapon hash");
                    return false;
                }

                const auto unlock = GetWeaponUnlock(weapon);
                if (unlock == 0U) {
                    Log("[ERROR][SHOP_UNLOCK] weapon-to-unlock resolver returned zero for Repeating Shotgun");
                    return false;
                }

                const auto beforeVisible =
                    UNLOCK::_0x8588A14B75AF096B(unlock) != FALSE;
                const auto beforeUnlocked =
                    UNLOCK::_0xC4B660C7B6040E75(unlock) != FALSE;
                const bool enable =
                    command == BridgeCommand::EnableRepeatingShotgunShopUnlock;
                if (enable) {
                    UNLOCK::_0x46B901A8ECDB5A61(unlock, TRUE);
                    UNLOCK::_0x1B7C5ADA8A6910A0(unlock, TRUE);
                }
                const auto afterVisible =
                    UNLOCK::_0x8588A14B75AF096B(unlock) != FALSE;
                const auto afterUnlocked =
                    UNLOCK::_0xC4B660C7B6040E75(unlock) != FALSE;
                Log(
                    std::string{"[INFO][SHOP_UNLOCK] item=Repeating Shotgun, weaponHash="} +
                    std::to_string(weapon) +
                    ", unlockHash=" + std::to_string(unlock) +
                    ", mode=" + (enable ? "enable" : "probe") +
                    ", visible=" + std::to_string(beforeVisible ? 1 : 0) +
                    "->" + std::to_string(afterVisible ? 1 : 0) +
                    ", unlocked=" + std::to_string(beforeUnlocked ? 1 : 0) +
                    "->" + std::to_string(afterUnlocked ? 1 : 0));
                return !enable || (afterVisible && afterUnlocked);
            } catch (...) {
                Log("[ERROR][SHOP_UNLOCK] Repeating Shotgun unlock test raised an exception");
                return false;
            }
#else
            Log("[ERROR][SHOP_UNLOCK] native bindings are disabled; unlock test was not run");
            return false;
#endif
        }
        case BridgeCommand::ProbePoisonThrowingKnifePamphlet:
        case BridgeCommand::EnablePoisonThrowingKnifePamphlet: {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
            try {
                // Confirmed item-database identifier: DOCUMENT_PAMPHLET_POISON_THROWING_KNIFE.
                char itemName[] = "DOCUMENT_PAMPHLET_POISON_THROWING_KNIFE";
                const auto unlock = GAMEPLAY::GET_HASH_KEY(itemName);
                constexpr Hash kExpectedUnlock = 0x366089E7U;
                if (unlock != kExpectedUnlock) {
                    Log("[ERROR][RECIPE_UNLOCK] Poison Throwing Knife pamphlet hash mismatch");
                    return false;
                }
                const auto beforeVisible = UNLOCK::_0x8588A14B75AF096B(unlock) != FALSE;
                const auto beforeUnlocked = UNLOCK::_0xC4B660C7B6040E75(unlock) != FALSE;
                const bool enable = command == BridgeCommand::EnablePoisonThrowingKnifePamphlet;
                if (enable) {
                    UNLOCK::_0x46B901A8ECDB5A61(unlock, TRUE);
                    UNLOCK::_0x1B7C5ADA8A6910A0(unlock, TRUE);
                }
                const auto afterVisible = UNLOCK::_0x8588A14B75AF096B(unlock) != FALSE;
                const auto afterUnlocked = UNLOCK::_0xC4B660C7B6040E75(unlock) != FALSE;
                Log(std::string{"[RECIPE_UNLOCK] item=Poison Throwing Knife Pamphlet, unlockHash="} +
                    std::to_string(unlock) + ", mode=" + (enable ? "enable" : "probe") +
                    ", visible=" + std::to_string(beforeVisible ? 1 : 0) + "->" +
                    std::to_string(afterVisible ? 1 : 0) + ", unlocked=" +
                    std::to_string(beforeUnlocked ? 1 : 0) + "->" +
                    std::to_string(afterUnlocked ? 1 : 0));
                return !enable || (afterVisible && afterUnlocked);
            } catch (...) {
                Log("[ERROR][RECIPE_UNLOCK] Poison Throwing Knife pamphlet probe raised an exception");
                return false;
            }
#else
            Log("[ERROR][RECIPE_UNLOCK] native bindings are disabled; recipe probe was not run");
            return false;
#endif
        }
        case BridgeCommand::ToggleSoloTest:
        case BridgeCommand::ToggleGhostRecord:
        case BridgeCommand::ToggleGhostReplay:
        case BridgeCommand::ResyncEntities:
        case BridgeCommand::RetryCheckpoint:
        case BridgeCommand::ToggleDiagnostics:
        case BridgeCommand::SaveProblemMarker:
        case BridgeCommand::Unload:
            return true;
        case BridgeCommand::ToggleSoloOverride:
        case BridgeCommand::TeleportGuest:
        case BridgeCommand::TeleportToPlayer:
        case BridgeCommand::ResyncEquipment:
            return false;
        case BridgeCommand::StopSession:
            return true;
    }
    return false;
}

bool ScriptHookSdkFacade::ApplyNetworkCommand(
    const CommandPayload& command) noexcept {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
    // UNVERIFIED_NATIVE_BINDING: all calls below use official SDK wrappers,
    // but their runtime behavior still needs validation on pinned build 1491.50.
    switch (command.opcode) {
        case CommandOpcode::SpawnReplica: {
            if (!command.target.IsValid() ||
                !IsFinite(command.position) ||
                !std::isfinite(command.heading)) {
                return false;
            }
            if (const auto existing =
                    replicas_.FindLocal(command.target);
                existing.has_value()) {
                const bool exists =
                    ENTITY::DOES_ENTITY_EXIST(*existing) != FALSE;
                if (exists) {
                    remotePlayerId_ = command.target;
                }
                return exists;
            }
            const auto sourcePed = PLAYER::PLAYER_PED_ID();
            if (sourcePed == 0 ||
                ENTITY::DOES_ENTITY_EXIST(sourcePed) == FALSE) {
                return false;
            }
            const auto spawnPosition =
                GroundSafePosition(command.position);
            const auto sourceModel =
                ENTITY::GET_ENTITY_MODEL(sourcePed);
            Ped replica{};
            bool independentPuppet{};
            if (sourceModel != 0U &&
                STREAMING::IS_MODEL_VALID(sourceModel) != FALSE) {
                STREAMING::REQUEST_MODEL(sourceModel, FALSE);
                if (STREAMING::HAS_MODEL_LOADED(sourceModel) != FALSE) {
                    replica = PED::CREATE_PED(
                        sourceModel,
                        spawnPosition.x,
                        spawnPosition.y,
                        spawnPosition.z,
                        command.heading,
                        FALSE,
                        FALSE,
                        FALSE,
                        FALSE);
                    independentPuppet = replica != 0;
                }
            }
            if (replica == 0) {
                // Fail safely on an unexpected model-loading edge case. The
                // clone path retains the previous behaviour and is reported
                // in diagnostics so a 2-PC test cannot silently use it.
                replica = PED::CLONE_PED(
                    sourcePed,
                    command.heading,
                    FALSE,
                    TRUE);
            }
            if (replica == 0) {
                return false;
            }
            ENTITY::SET_ENTITY_AS_MISSION_ENTITY(replica, TRUE, TRUE);
            if (independentPuppet) {
                SetRandomOutfitVariation(replica);
            }
            // Keep the native ped audio/animation event graph enabled. The
            // direct-root replica still uses ordinary movement tasks, so RDR2
            // can emit its own surface-aware movement events where available.
            PED::SET_PED_CAN_PLAY_AMBIENT_ANIMS(replica, TRUE);
            PED::SET_PED_CAN_PLAY_AMBIENT_BASE_ANIMS(replica, TRUE);
            ENTITY::SET_ENTITY_LOAD_COLLISION_FLAG(replica, TRUE);
            ENTITY::SET_ENTITY_HAS_GRAVITY(replica, TRUE);
            ENTITY::SET_ENTITY_COLLISION(replica, TRUE, TRUE);
            // The peer replica is a visual/targetable stand-in. Local bullets
            // may hit it, but health authority always remains remote.
            ENTITY::SET_ENTITY_CAN_BE_DAMAGED(replica, FALSE);
            ENTITY::SET_ENTITY_VISIBLE(replica, TRUE);
            ENTITY::RESET_ENTITY_ALPHA(replica);
            ENTITY::SET_ENTITY_CAN_BE_TARGETED_WITHOUT_LOS(
                replica,
                TRUE);
            PED::SET_PED_CAN_BE_TARGETTED(replica, TRUE);
            PED::SET_PED_CAN_BE_TARGETTED_BY_PLAYER(
                replica,
                PLAYER::PLAYER_ID(),
                TRUE);
            PED::SET_PED_CAN_RAGDOLL(replica, TRUE);
            PED::REGISTER_TARGET(sourcePed, replica, TRUE);
            // A fallback clone inherits the local player's friendly
            // relationship group. Independent puppets also use the private
            // group so both spawn paths have identical targeting policy.
            const auto localRelationship =
                PED::GET_PED_RELATIONSHIP_GROUP_HASH(
                    sourcePed);
            if (remoteRelationshipGroup_ == 0U) {
                char remoteRelationshipName[] =
                    "R2CP_REMOTE_PLAYER";
                Hash createdRelationship{};
                (void)PED::ADD_RELATIONSHIP_GROUP(
                    remoteRelationshipName,
                    &createdRelationship);
                remoteRelationshipGroup_ =
                    static_cast<std::uint32_t>(
                        createdRelationship);
            }
            if (remoteRelationshipGroup_ != 0U) {
                if (remoteRelationshipLocalGroup_ != 0U &&
                    remoteRelationshipLocalGroup_ !=
                        localRelationship) {
                    PED::CLEAR_RELATIONSHIP_BETWEEN_GROUPS(
                        5,
                        remoteRelationshipLocalGroup_,
                        remoteRelationshipGroup_);
                    PED::CLEAR_RELATIONSHIP_BETWEEN_GROUPS(
                        5,
                        remoteRelationshipGroup_,
                        remoteRelationshipLocalGroup_);
                }
                PED::SET_PED_RELATIONSHIP_GROUP_DEFAULT_HASH(
                    replica,
                    remoteRelationshipGroup_);
                PED::SET_PED_RELATIONSHIP_GROUP_HASH(
                    replica,
                    remoteRelationshipGroup_);
                if (localRelationship != 0U) {
                    PED::SET_RELATIONSHIP_BETWEEN_GROUPS(
                        5,
                        localRelationship,
                        remoteRelationshipGroup_);
                    PED::SET_RELATIONSHIP_BETWEEN_GROUPS(
                        5,
                        remoteRelationshipGroup_,
                        localRelationship);
                    ENTITY::SET_ENTITY_CAN_BE_DAMAGED_BY_RELATIONSHIP_GROUP(
                        sourcePed,
                        FALSE,
                        remoteRelationshipGroup_);
                    remoteRelationshipLocalGroup_ =
                        localRelationship;
                }
            }
            ENTITY::SET_ENTITY_COORDS_NO_OFFSET(
                replica,
                spawnPosition.x,
                spawnPosition.y,
                spawnPosition.z,
                FALSE,
                FALSE,
                FALSE);
            ENTITY::SET_ENTITY_HEADING(replica, command.heading);
            ENTITY::SET_ENTITY_VELOCITY(
                replica,
                0.0F,
                0.0F,
                0.0F);
            PED::SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(replica, TRUE);
            PED::SET_PED_KEEP_TASK(replica, TRUE);
            Log(
                std::string{
                    independentPuppet
                        ? "[INFO][PUPPET_SPAWN] independent CREATE_PED proxy; outfit initialized"
                        : "[WARN][PUPPET_SPAWN] CREATE_PED unavailable; CLONE_PED fallback used"} +
                ", visible=" +
                std::to_string(
                    ENTITY::IS_ENTITY_VISIBLE(replica) != FALSE) +
                ", visible-to-script=" +
                std::to_string(
                    ENTITY::IS_ENTITY_VISIBLE_TO_SCRIPT(replica) != FALSE) +
                ", alpha=" +
                std::to_string(ENTITY::GET_ENTITY_ALPHA(replica)));
            if (!replicas_.Bind(command.target, replica)) {
                PED::DELETE_PED(&replica);
                return false;
            }
            remotePlayerId_ = command.target;
            remoteAppearanceFingerprint_ = 0U;
            remoteAppearanceModelHash_ = 0U;
            remoteAppearanceComponents_.clear();
            remoteWeaponHash_ = 0U;
            remoteReloading_ = false;
            ResetRemoteMotionTracking();
            previousRemoteTaskDestination_ = spawnPosition;
            return true;
        }
        case CommandOpcode::ApplyTransform: {
            return ApplyRemoteTransform(
                PlayerStatePayload{
                    command.target,
                    PlayerSlot::Guest,
                    PlayerLifecycle::Alive,
                    command.position,
                    {},
                    command.heading,
                    1.0F,
                    command.flags});
        }
        case CommandOpcode::DespawnReplica: {
            const auto handle = replicas_.FindLocal(command.target);
            if (!handle.has_value()) {
                if (command.target == remotePlayerId_) {
                    ReleaseRemoteGunshotAudio();
                    ClearRemoteIdentityDecoration();
                    remoteWeaponHash_ = 0U;
                    remoteReloading_ = false;
                    authoritativeRemoteRestraint_ =
                        PlayerRestraintState::Free;
                    authoritativeRemoteRestraintSubject_ = NetEntityId{};
                    authoritativeRemoteRestraintRevision_ = 0U;
                    authoritativeRemoteRestraintReceivedMs_ = 0U;
                    previousAuthoritativeRemoteRestraintRagdollMs_ = 0U;
                    previousRemoteDownedRagdollMs_ = 0U;
                    remoteAppearanceFingerprint_ = 0U;
                    remoteAppearanceModelHash_ = 0U;
                    remoteAppearanceComponents_.clear();
                    remotePlayerId_ = NetEntityId{};
                    ResetRemoteMotionTracking();
                }
                return true;
            }
            auto ped = static_cast<Ped>(*handle);
            (void)replicas_.Remove(command.target);
            Log(
                "[INFO][PUPPET_DESPAWN] begin stable nameplate and blip cleanup");
            if (command.target == remotePlayerId_) {
                ReleaseRemoteGunshotAudio();
                ClearRemoteIdentityDecoration();
            }
            if (ENTITY::DOES_ENTITY_EXIST(ped) != FALSE) {
                PED::DELETE_PED(&ped);
            }
            Log(
                "[INFO][PUPPET_DESPAWN] completed without native gamer-tag calls");
            if (command.target == remotePlayerId_) {
                remoteWeaponHash_ = 0U;
                remoteReloading_ = false;
                authoritativeRemoteRestraint_ =
                    PlayerRestraintState::Free;
                authoritativeRemoteRestraintSubject_ = NetEntityId{};
                authoritativeRemoteRestraintRevision_ = 0U;
                authoritativeRemoteRestraintReceivedMs_ = 0U;
                previousAuthoritativeRemoteRestraintRagdollMs_ = 0U;
                previousRemoteDownedRagdollMs_ = 0U;
                remoteAppearanceFingerprint_ = 0U;
                remoteAppearanceModelHash_ = 0U;
                remoteAppearanceComponents_.clear();
                remotePlayerId_ = NetEntityId{};
                ResetRemoteMotionTracking();
            }
            return true;
        }
        case CommandOpcode::TeleportGuest: {
            if (!command.target.IsValid() ||
                !IsFinite(command.position) ||
                !std::isfinite(command.heading)) {
                Log(
                    "teleport guest native rejected an invalid payload");
                return false;
            }
            const auto localPed = PLAYER::PLAYER_PED_ID();
            if (localPed == 0 ||
                ENTITY::DOES_ENTITY_EXIST(localPed) == FALSE) {
                Log(
                    "teleport guest native failed: local ped is unavailable");
                return false;
            }

            const auto safePosition =
                TeleportSafePosition(command.position);
            Entity teleportRoot = static_cast<Entity>(localPed);
            bool mountedRoot{};
            if (PED::IS_PED_ON_MOUNT(localPed) != FALSE) {
                const auto currentMount = PED::GET_MOUNT(localPed);
                if (currentMount != 0 &&
                    ENTITY::DOES_ENTITY_EXIST(currentMount) != FALSE) {
                    teleportRoot = static_cast<Entity>(currentMount);
                    mountedRoot = true;
                }
            }
            ENTITY::SET_ENTITY_LOAD_COLLISION_FLAG(
                teleportRoot,
                TRUE);
            ENTITY::SET_ENTITY_COORDS_NO_OFFSET(
                teleportRoot,
                safePosition.x,
                safePosition.y,
                safePosition.z,
                FALSE,
                FALSE,
                FALSE);
            ENTITY::SET_ENTITY_VELOCITY(
                teleportRoot,
                0.0F,
                0.0F,
                0.0F);
            ENTITY::SET_ENTITY_HEADING(teleportRoot, command.heading);
            if (mountedRoot) {
                // Prevent residual rider velocity from fighting the mounted
                // root transform during the following physics step.
                ENTITY::SET_ENTITY_VELOCITY(
                    localPed,
                    0.0F,
                    0.0F,
                    0.0F);
            }
            Log(
                "teleport guest native applied to " +
                (mountedRoot
                     ? std::string{"mounted root"}
                     : std::string{"player root"}) +
                " at (" +
                std::to_string(safePosition.x) + ", " +
                std::to_string(safePosition.y) + ", " +
                std::to_string(safePosition.z) + ")");
            return true;
        }
        case CommandOpcode::EnterDowned: {
            const auto handle = replicas_.FindLocal(command.target);
            if (!handle.has_value()) {
                return false;
            }
            return PED::SET_PED_TO_RAGDOLL(
                       *handle,
                       -1,
                       -1,
                       0,
                       FALSE,
                       FALSE,
                       FALSE) != FALSE;
        }
        case CommandOpcode::CompleteRevive: {
            const auto handle = replicas_.FindLocal(command.target);
            if (!handle.has_value()) {
                return false;
            }
            AI::CLEAR_PED_TASKS_IMMEDIATELY(
                *handle,
                FALSE,
                TRUE);
            return true;
        }
        case CommandOpcode::SpectatorOn:
        case CommandOpcode::SpectatorOff:
        case CommandOpcode::SoloOverrideOn:
        case CommandOpcode::SoloOverrideOff:
        case CommandOpcode::ResyncEquipment:
            return false;
        case CommandOpcode::Resync:
        case CommandOpcode::RetryCheckpoint:
        case CommandOpcode::Unload:
        case CommandOpcode::ToggleDiagnostics:
            return true;
    }
#else
    (void)command;
#endif
    return false;
}

bool ScriptHookSdkFacade::ApplyRemoteTraversal(
    const PlayerTraversalPayload& traversal) noexcept {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
    if (!traversal.entityId.IsValid() ||
        traversal.kind == PlayerTraversalKind::None ||
        traversal.actionId == 0U || traversal.revision == 0U ||
        traversal.locomotionEpoch == 0U ||
        !IsFinite(traversal.takeoffPosition) ||
        !IsFinite(traversal.approachVelocity)) {
        return false;
    }
    if (remotePlayerId_.IsValid() &&
        traversal.entityId != remotePlayerId_) {
        return false;
    }

    const auto existing = std::find_if(
        pendingRemoteTraversals_.begin(),
        pendingRemoteTraversals_.end(),
        [&](const RemoteTraversalIntent& candidate) noexcept {
            return candidate.actionId == traversal.actionId;
        });
    if (existing != pendingRemoteTraversals_.end()) {
        if (traversal.revision <= existing->revision) {
            return true;
        }
        existing->kind =
            traversal.kind == PlayerTraversalKind::Climb
                ? RemoteTraversalKind::Climb
                : RemoteTraversalKind::Jump;
        existing->position = traversal.takeoffPosition;
        existing->heading = traversal.takeoffHeading;
        existing->revision = traversal.revision;
        existing->locomotionEpoch = traversal.locomotionEpoch;
        existing->flags = traversal.flags;
        existing->approachVelocity = traversal.approachVelocity;
        existing->obstaclePoint = traversal.obstaclePoint;
        existing->obstacleNormal = traversal.obstacleNormal;
        existing->obstacleTopZ = traversal.obstacleTopZ;
        existing->expectedLanding = traversal.expectedLanding;
        existing->geometryProbeHandle = 0;
        existing->geometryProbeStartedMs = 0U;
        existing->geometryProbeComplete = false;
        existing->geometryConfirmed = false;
        ++remoteActionTraversalReliableUpdates_;
        return true;
    }
    if (traversal.actionId == lastRemoteTraversalActionId_) {
        // The initial revision may already have committed before the sender's
        // landing revision arrived. Never turn that update into a second
        // climb at the landing point.
        ++remoteActionTraversalReliableUpdates_;
        return true;
    }

    constexpr std::size_t kMaximumPendingTraversals = 8U;
    if (pendingRemoteTraversals_.size() >=
        kMaximumPendingTraversals) {
        pendingRemoteTraversals_.pop_front();
        ++remoteActionTraversalExpired_;
    }
    RemoteTraversalIntent intent;
    intent.kind =
        traversal.kind == PlayerTraversalKind::Climb
            ? RemoteTraversalKind::Climb
            : RemoteTraversalKind::Jump;
    intent.position = traversal.takeoffPosition;
    intent.heading = traversal.takeoffHeading;
    intent.actionId = traversal.actionId;
    intent.capturedAtMs = TickMilliseconds();
    intent.revision = traversal.revision;
    intent.locomotionEpoch = traversal.locomotionEpoch;
    intent.flags = traversal.flags;
    intent.approachVelocity = traversal.approachVelocity;
    intent.obstaclePoint = traversal.obstaclePoint;
    intent.obstacleNormal = traversal.obstacleNormal;
    intent.obstacleTopZ = traversal.obstacleTopZ;
    intent.expectedLanding = traversal.expectedLanding;
    pendingRemoteTraversals_.push_back(intent);
    lastRemoteTraversalActionId_ = traversal.actionId;
    ++remoteActionTraversalDeferred_;
    ++remoteActionTraversalReliableUpdates_;
    Log(
        "[INFO][PUPPET_TRAVERSAL] reliable transaction queued: id=" +
        std::to_string(traversal.actionId) +
        ", revision=" +
        std::to_string(traversal.revision) +
        ", pre-takeoff=" +
        (((traversal.flags &
           static_cast<std::uint32_t>(
               PlayerTraversalFlag::InputEdgeDetected)) != 0U)
             ? "1"
             : "0"));
    return true;
#else
    (void)traversal;
    return false;
#endif
}

bool ScriptHookSdkFacade::ApplyRemotePlayerAction(
    const PlayerActionPayload& action) noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        if (!action.actorEntityId.IsValid() ||
            action.actionId == 0U || action.revision == 0U ||
            action.kind == PlayerActionKind::None ||
            action.phase == PlayerActionPhase::None ||
            (remotePlayerId_.IsValid() &&
             action.actorEntityId != remotePlayerId_)) {
            return false;
        }
        const auto index = static_cast<std::size_t>(action.kind);
        if (index >= remotePlayerActionChannels_.size()) {
            return false;
        }

        constexpr auto kTargetEntityValid = static_cast<std::uint32_t>(
            PlayerActionFlag::TargetEntityValid);
        constexpr auto kPhysicalTargetEffect = static_cast<std::uint32_t>(
            PlayerActionFlag::PhysicalTargetEffect);
        const bool targetsLocalPlayer =
            (action.flags & kTargetEntityValid) != 0U &&
            action.targetEntityId.IsValid();
        const bool appliesPhysicalTargetEffect =
            targetsLocalPlayer &&
            (action.flags & kPhysicalTargetEffect) != 0U;
        const bool terminal =
            IsTerminalPlayerActionPhase(action.phase);
        auto& channel = remotePlayerActionChannels_[index];
        const auto epochDecision = EvaluateRemotePlayerActionEpoch(
            channel.actionId,
            channel.revision,
            action.actionId,
            action.revision,
            action.phase);
        if (!AcceptsRemotePlayerActionEpoch(epochDecision)) {
            if (epochDecision ==
                RemotePlayerActionEpochDecision::IgnoreStaleRevision) {
                ++remotePlayerActionStaleRevisions_;
            } else {
                ++remotePlayerActionEpochRejects_;
                if (epochDecision ==
                    RemotePlayerActionEpochDecision::IgnoreForeignTerminal) {
                    ++remotePlayerActionForeignTerminals_;
                }
                Log(
                    "[WARNING][ACTION_EPOCH] ignored out-of-epoch packet: kind=" +
                    std::to_string(
                        static_cast<std::uint8_t>(action.kind)) +
                    ", incoming-action-id=" +
                    std::to_string(action.actionId) +
                    ", current-action-id=" +
                    std::to_string(channel.actionId) +
                    ", phase=" +
                    std::to_string(
                        static_cast<std::uint8_t>(action.phase)) +
                    ", decision=" +
                    std::to_string(
                        static_cast<std::uint8_t>(epochDecision)));
            }
            return true;
        }

        const bool newAction =
            epochDecision ==
                RemotePlayerActionEpochDecision::AcceptInitial ||
            epochDecision ==
                RemotePlayerActionEpochDecision::AcceptNewBegin;
        const bool targetAcquiredThisRevision =
            targetsLocalPlayer &&
            (newAction || !channel.targetsLocalPlayer);
        const auto actor =
            replicas_.FindLocal(action.actorEntityId);
        if (newAction && channel.active) {
            ++remotePlayerActionPreemptions_;
            if (action.kind == PlayerActionKind::Lasso ||
                action.kind == PlayerActionKind::Hogtie) {
                DeleteRemotePeerLassoRope();
            }
            if ((action.kind == PlayerActionKind::MeleeAttack ||
                 action.kind == PlayerActionKind::Grapple) &&
                actor.has_value() &&
                ENTITY::DOES_ENTITY_EXIST(*actor) != FALSE) {
                CancelRemoteMeleeVisual(
                    *actor,
                    "new-action-preemption");
            }
        }

        const auto deactivateConflictingChannel =
            [&](const PlayerActionKind kind) noexcept {
                const auto conflictingIndex =
                    static_cast<std::size_t>(kind);
                if (conflictingIndex >=
                    remotePlayerActionChannels_.size()) {
                    return;
                }
                auto& conflicting =
                    remotePlayerActionChannels_[conflictingIndex];
                if (!conflicting.active) {
                    return;
                }
                conflicting.active = false;
                conflicting.phase = PlayerActionPhase::Cancel;
                conflicting.targetsLocalPlayer = false;
                conflicting.physicalTargetEffect = false;
                ++remotePlayerActionPreemptions_;
            };
        if (!terminal &&
            IsPlayerActionEpochStartPhase(action.phase)) {
            switch (action.kind) {
                case PlayerActionKind::MeleeAttack:
                    deactivateConflictingChannel(
                        PlayerActionKind::MeleeBlock);
                    deactivateConflictingChannel(
                        PlayerActionKind::Grapple);
                    break;
                case PlayerActionKind::MeleeBlock:
                    deactivateConflictingChannel(
                        PlayerActionKind::MeleeAttack);
                    deactivateConflictingChannel(
                        PlayerActionKind::Grapple);
                    CancelRemoteMeleeVisual(
                        actor.value_or(0),
                        "block-preemption");
                    break;
                case PlayerActionKind::Grapple:
                    deactivateConflictingChannel(
                        PlayerActionKind::MeleeAttack);
                    deactivateConflictingChannel(
                        PlayerActionKind::MeleeBlock);
                    CancelRemoteMeleeVisual(
                        actor.value_or(0),
                        "grapple-preemption");
                    break;
                case PlayerActionKind::Lasso:
                case PlayerActionKind::Hogtie:
                    deactivateConflictingChannel(
                        PlayerActionKind::MeleeAttack);
                    deactivateConflictingChannel(
                        PlayerActionKind::Grapple);
                    CancelRemoteMeleeVisual(
                        actor.value_or(0),
                        "lasso-preemption");
                    break;
                case PlayerActionKind::Aim:
                case PlayerActionKind::Knockdown:
                case PlayerActionKind::Crafting:
                case PlayerActionKind::None:
                    break;
            }
        }

        channel.actionId = action.actionId;
        channel.revision = action.revision;
        channel.phase = action.phase;
        channel.receivedAtMs = TickMilliseconds();
        channel.active = !terminal;
        channel.targetsLocalPlayer =
            !terminal && targetsLocalPlayer;
        channel.physicalTargetEffect =
            !terminal && targetsLocalPlayer &&
            (action.flags & kPhysicalTargetEffect) != 0U;
        reliablePlayerActionProtocolObserved_ = true;
        if (action.phase == PlayerActionPhase::Begin || newAction) {
            ++remotePlayerActionBegins_;
        } else if (action.phase == PlayerActionPhase::Sustain ||
                   action.phase == PlayerActionPhase::Active ||
                   action.phase == PlayerActionPhase::Snapshot) {
            ++remotePlayerActionSustains_;
        }
        if (terminal) {
            ++remotePlayerActionTerminals_;
        }
        RefreshRemotePlayerActionDerivedState();

        // EquipmentState normally arrives alongside Aim, but a reliable
        // action can beat that periodic snapshot. Utility weapons are also
        // reported as invalid by IS_WEAPON_VALID in parts of the prologue.
        // Prime the remote actor directly from the authenticated action so
        // the native lasso graph has a real weapon before aiming/throwing.
        if ((action.kind == PlayerActionKind::Aim ||
             action.kind == PlayerActionKind::Lasso ||
             action.kind == PlayerActionKind::Hogtie) &&
            action.weaponHash == kWeaponLasso &&
            actor.has_value() &&
            ENTITY::DOES_ENTITY_EXIST(*actor) != FALSE) {
            remoteWeaponHash_ = kWeaponLasso;
            remoteWeaponAmmo_ = 1U;
            const auto lasso = static_cast<Hash>(kWeaponLasso);
            if (WEAPON::HAS_PED_GOT_WEAPON(
                    *actor,
                    lasso,
                    FALSE,
                    FALSE) == FALSE) {
                WEAPON::GIVE_DELAYED_WEAPON_TO_PED(
                    *actor,
                    lasso,
                    1,
                    TRUE,
                    0);
                ++remoteActionWeaponGrants_;
            }
            WEAPON::SET_PED_AMMO(*actor, lasso, 1);
            WEAPON::SET_CURRENT_PED_WEAPON(
                *actor,
                lasso,
                TRUE,
                0,
                FALSE,
                FALSE);
            remoteWeaponConfirmedOwned_ = true;
            remoteWeaponNextGrantMs_ = 0U;
            remoteWeaponGrantAttempts_ = 0U;
        }

        if (terminal) {
            // The sender normally publishes its melee terminal just before
            // the receiver reaches impact. Do not clear the native strike at
            // that point: its bounded visual deadline (or the next action)
            // owns cleanup. V29.5 cancelled every punch at contact.
            if (action.kind == PlayerActionKind::Aim &&
                !remoteActionLassoActive_ &&
                actor.has_value() &&
                ENTITY::DOES_ENTITY_EXIST(*actor) != FALSE) {
                AI::CLEAR_PED_SECONDARY_TASK(*actor);
                previousRemoteAimTaskMs_ = 0U;
                animGraphVisualTaskActive_ = false;
                ++remotePlayerActionNativeCancels_;
            }
            if (action.kind == PlayerActionKind::Lasso ||
                action.kind == PlayerActionKind::Hogtie) {
                if (actor.has_value() &&
                    ENTITY::DOES_ENTITY_EXIST(*actor) != FALSE) {
                    AI::CLEAR_PED_TASKS_IMMEDIATELY(
                        *actor,
                        FALSE,
                        TRUE);
                    PED::SET_PED_KEEP_TASK(*actor, FALSE);
                }
                DeleteRemotePeerLassoRope();
                ++remotePlayerActionNativeCancels_;
            }
            if (action.kind == PlayerActionKind::Crafting &&
                actor.has_value() &&
                ENTITY::DOES_ENTITY_EXIST(*actor) != FALSE) {
                // This is a bridge-owned puppet task, never the local
                // player's crafting UI or inventory transaction.
                AI::CLEAR_PED_TASKS(*actor, FALSE, FALSE);
                PED::SET_PED_KEEP_TASK(*actor, FALSE);
                ++remotePlayerActionNativeCancels_;
            }
            Log(
                "[ACTION_FSM] terminal kind=" +
                std::to_string(static_cast<std::uint8_t>(action.kind)) +
                ", phase=" +
                std::to_string(static_cast<std::uint8_t>(action.phase)) +
                ", action-id=" + std::to_string(action.actionId) +
                ", revision=" + std::to_string(action.revision));
            return true;
        }

        if (newAction &&
            action.phase == PlayerActionPhase::Begin &&
            action.kind == PlayerActionKind::Crafting) {
            bool scenarioStarted{};
            if (actor.has_value() &&
                ENTITY::DOES_ENTITY_EXIST(*actor) != FALSE &&
                (action.flags & static_cast<std::uint32_t>(
                     PlayerActionFlag::ActorAnchorValid)) != 0U &&
                IsFinite(action.actorAnchor) &&
                PED::IS_PED_ON_MOUNT(*actor) == FALSE &&
                PED::IS_PED_RAGDOLL(*actor) == FALSE) {
                constexpr float kCraftingScenarioRadiusMeters = 2.5F;
                if (AI::DOES_SCENARIO_EXIST_IN_AREA(
                        action.actorAnchor.x,
                        action.actorAnchor.y,
                        action.actorAnchor.z,
                        kCraftingScenarioRadiusMeters,
                        TRUE,
                        0,
                        FALSE) != FALSE) {
                    AI::TASK_USE_NEAREST_SCENARIO_TO_COORD_WARP(
                        *actor,
                        action.actorAnchor.x,
                        action.actorAnchor.y,
                        action.actorAnchor.z,
                        kCraftingScenarioRadiusMeters,
                        0,
                        TRUE,
                        FALSE,
                        FALSE,
                        FALSE);
                    PED::SET_PED_KEEP_TASK(*actor, TRUE);
                    scenarioStarted = true;
                    Log("[CRAFTING_ACTIVITY] remote scenario presentation started");
                }
            }
            if (!scenarioStarted) {
                Log("[CRAFTING_ACTIVITY] semantic activity accepted; no matching local scenario");
            }
        } else if (newAction &&
            action.phase == PlayerActionPhase::Begin &&
            action.kind == PlayerActionKind::MeleeAttack) {
            bool visualStarted{};
            if (targetsLocalPlayer && actor.has_value() &&
                ENTITY::DOES_ENTITY_EXIST(*actor) != FALSE) {
                const auto localPed = PLAYER::PLAYER_PED_ID();
                if (localPed != 0 &&
                    ENTITY::DOES_ENTITY_EXIST(localPed) != FALSE &&
                    PED::IS_PED_ON_MOUNT(*actor) == FALSE &&
                    PED::IS_PED_ON_MOUNT(localPed) == FALSE &&
                    PED::IS_PED_RAGDOLL(*actor) == FALSE &&
                    PED::IS_PED_FALLING(*actor) == FALSE &&
                    AI::IS_PED_GETTING_UP(*actor) == FALSE) {
                    const auto actorPosition = ToBridgeVector(
                        ENTITY::GET_ENTITY_COORDS(
                            *actor,
                            TRUE,
                            FALSE));
                    const auto targetPosition = ToBridgeVector(
                        ENTITY::GET_ENTITY_COORDS(
                            localPed,
                            TRUE,
                            FALSE));
                    if (IsFinite(actorPosition) &&
                        IsFinite(targetPosition) &&
                        Distance(actorPosition, targetPosition) <=
                            kPeerMountPullMaximumDistanceMeters) {
                        CancelRemoteMeleeVisual(
                            *actor,
                            "next-melee-begin");
                        ENTITY::SET_ENTITY_HEADING(
                            *actor,
                            action.facingHeading);
                        AI::TASK_PUT_PED_DIRECTLY_INTO_MELEE(
                            *actor,
                            localPed,
                            0.0F,
                            -1.0F,
                            0.0F,
                            0.0F,
                            0);
                        PED::SET_PED_KEEP_TASK(*actor, TRUE);
                        remoteMeleeVisualActionId_ = action.actionId;
                        const auto requestedDuration =
                            action.durationMilliseconds == 0U
                                ? kRemoteMeleeVisualMaximumMilliseconds
                                : static_cast<std::uint64_t>(
                                      action.durationMilliseconds);
                        remoteMeleeVisualDeadlineMs_ =
                            channel.receivedAtMs +
                            std::min(
                                requestedDuration,
                                kRemoteMeleeVisualMaximumMilliseconds);
                        previousRemoteMeleeTaskMs_ =
                            channel.receivedAtMs;
                        ++remotePlayerActionMeleeVisualStarts_;
                        if (motionReplicationMode_ ==
                            MotionReplicationWireMode::AnimGraphReplica) {
                            ++animGraphMeleeTaskStarts_;
                        }
                        visualStarted = true;
                        Log(
                            "[MELEE_VISUAL] one bounded native strike started: action-id=" +
                            std::to_string(action.actionId) +
                            ", deadline-ms=" +
                            std::to_string(
                                remoteMeleeVisualDeadlineMs_ -
                                channel.receivedAtMs) +
                            ", refresh-policy=never");
                    }
                }
            }
            if (!visualStarted) {
                ++remotePlayerActionMeleeSemanticOnly_;
                Log(
                    "[MELEE_VISUAL] attack accepted as semantic-only (no close valid peer target): action-id=" +
                    std::to_string(action.actionId));
            }
        } else if (
            newAction &&
            action.phase == PlayerActionPhase::Begin &&
            (action.kind == PlayerActionKind::MeleeBlock ||
             action.kind == PlayerActionKind::Grapple)) {
            ++remotePlayerActionMeleeSemanticOnly_;
            Log(
                "[MELEE_VISUAL] block/grapple is semantic-only; autonomous combo task suppressed: kind=" +
                std::to_string(
                    static_cast<std::uint8_t>(action.kind)) +
                ", action-id=" +
                std::to_string(action.actionId));
        }

        if (newAction &&
            action.phase == PlayerActionPhase::Begin &&
            action.kind == PlayerActionKind::Knockdown &&
            appliesPhysicalTargetEffect) {
            constexpr auto kVariantValid =
                static_cast<std::uint32_t>(
                    PlayerActionFlag::VariantValid);
            const bool deliberatePeerMountPull =
                (action.flags & kVariantValid) != 0U &&
                action.variantHash ==
                    kPlayerActionVariantPeerMountPull;
            const auto localPed = PLAYER::PLAYER_PED_ID();
            if (localPed != 0 &&
                ENTITY::DOES_ENTITY_EXIST(localPed) != FALSE) {
                const auto now = channel.receivedAtMs;
                if (PED::IS_PED_ON_MOUNT(localPed) != FALSE) {
                    if (deliberatePeerMountPull) {
                        TaskDismountAnimal(localPed);
                        remotePeerDismountActionId_ = action.actionId;
                        remotePeerDismountStartedMs_ = now;
                        remotePeerDismountLastAttemptMs_ = now;
                        remotePeerDismountAttempts_ = 1U;
                        // This transaction is an explicitly tagged mount
                        // pull, not a generic fall or an overlapping vanilla
                        // context input.
                        previousPeerLassoRagdollMs_ = now;
                        ++remotePlayerActionDismountRequests_;
                        Log(
                            "[PEER_DISMOUNT] explicit victim-owned local dismount requested once: action-id=" +
                            std::to_string(action.actionId));
                        Log(
                            "[MOUNT_LIFECYCLE] direction=rx, state=dismount-requested, correlation=action-" +
                            std::to_string(action.actionId) +
                            ", action-id=" +
                            std::to_string(action.actionId) +
                            ", reason=tagged-peer-mount-pull");
                    } else {
                        Log(
                            "[MOUNT_INPUT_ISOLATION] ignored an untagged Knockdown while the local player was mounted");
                    }
                } else if (
                    PED::SET_PED_TO_RAGDOLL(
                        localPed,
                        kPeerKnockdownRagdollDurationMilliseconds,
                        kPeerKnockdownRagdollDurationMilliseconds,
                        0,
                        TRUE,
                        TRUE,
                        FALSE) != FALSE) {
                    previousPeerLassoRagdollMs_ = now;
                    ++remotePlayerActionVictimFallbacks_;
                    Log(
                        "[VICTIM_CONSTRAINT] authoritative zero-damage knockdown applied directly from Begin");
                }
            }
        }
        if (ShouldStartNativeLassoTask(
                action.kind,
                action.phase,
                targetsLocalPlayer,
                targetAcquiredThisRevision,
                appliesPhysicalTargetEffect) &&
            actor.has_value() &&
            ENTITY::DOES_ENTITY_EXIST(*actor) != FALSE) {
            const auto localPed = PLAYER::PLAYER_PED_ID();
            if (localPed != 0 &&
                ENTITY::DOES_ENTITY_EXIST(localPed) != FALSE) {
                const auto now = TickMilliseconds();
                if (remotePeerLassoRopeActionId_ == action.actionId &&
                    remotePeerLassoTaskPending_) {
                    const bool engineOwnsConstraint =
                        GetLassoTarget(static_cast<Ped>(*actor)) == localPed ||
                        IsPedLassoed(localPed) != FALSE ||
                        IsPedBeingHogtied(localPed) != FALSE ||
                        IsPedHogtied(localPed) != FALSE;
                    if (engineOwnsConstraint) {
                        remotePeerLassoTaskPending_ = false;
                        remotePeerLassoEngineOwned_ = true;
                        ++remotePlayerActionRopeCreates_;
                        ++remotePlayerActionLassoConfirmed_;
                        Log(
                            "[LASSO_ROPE] engine confirmed native constraint for action-id=" +
                            std::to_string(action.actionId));
                        Log(
                            "[LASSO_LIFECYCLE] direction=rx, state=constraint-confirmed, correlation=action-" +
                            std::to_string(action.actionId) +
                            ", action-id=" +
                            std::to_string(action.actionId) +
                            ", reason=engine-ownership-observed");
                    } else if (
                        now >= remotePeerLassoTaskStartedMs_ &&
                        now - remotePeerLassoTaskStartedMs_ >= 1'000U) {
                        if (remotePeerLassoTaskAttempts_ < 2U) {
                            TaskLassoPed(
                                static_cast<Ped>(*actor),
                                localPed);
                            PED::SET_PED_KEEP_TASK(*actor, TRUE);
                            remotePeerLassoTaskStartedMs_ = now;
                            ++remotePeerLassoTaskAttempts_;
                            ++remotePlayerActionLassoPending_;
                            Log(
                                "[WARNING][LASSO_ROPE] native constraint not observed; one controlled retry for action-id=" +
                                std::to_string(action.actionId));
                            Log(
                                "[LASSO_LIFECYCLE] direction=rx, state=retry, correlation=action-" +
                                std::to_string(action.actionId) +
                                ", action-id=" +
                                std::to_string(action.actionId) +
                                ", reason=engine-constraint-not-observed");
                        } else if (
                            remotePeerLassoTaskAttempts_ == 2U) {
                            // The native query is not a reliable completion
                            // signal for a player-owned victim in every Story
                            // state. Keep the task as visual owner until the
                            // authenticated terminal instead of handing the
                            // ped back to root/aim tasks after two seconds;
                            // that handoff made the lasso vanish from the
                            // remote actor's hands in V29.5.
                            remotePeerLassoTaskAttempts_ = 3U;
                            remotePeerLassoEngineOwned_ = false;
                            ++remotePlayerActionLassoFailed_;
                            Log(
                                "[WARNING][LASSO_ROPE] native constraint not confirmed; retaining lasso task ownership until terminal and refusing fake ragdoll, action-id=" +
                                std::to_string(action.actionId));
                            Log(
                                "[LASSO_LIFECYCLE] direction=rx, state=visual-owner-retained, correlation=action-" +
                                std::to_string(action.actionId) +
                                ", action-id=" +
                                std::to_string(action.actionId) +
                                ", reason=engine-constraint-query-not-confirmed");
                        }
                    }
                }
                if (remotePeerLassoRopeActionId_ == action.actionId) {
                    Log(
                        "[ACTION_FSM] apply kind=" +
                        std::to_string(static_cast<std::uint8_t>(action.kind)) +
                        ", phase=" +
                        std::to_string(static_cast<std::uint8_t>(action.phase)) +
                        ", action-id=" + std::to_string(action.actionId) +
                        ", revision=" + std::to_string(action.revision) +
                        ", target-effect=1");
                    return true;
                }

                // A Begin packet can precede target acquisition. Start the
                // engine-owned rope on the first revision that names the
                // local victim, not merely on the first action revision.
                DeleteRemotePeerLassoRope();
                constexpr std::array<int, 8> kManagedFlags{
                    0, 2, 3, 4, 7, 8, 9, 11};
                if (!localLassoFlagsCaptured_) {
                    for (std::size_t flagIndex{};
                         flagIndex < kManagedFlags.size();
                         ++flagIndex) {
                        localLassoOriginalFlags_[flagIndex] =
                            GetPedLassoHogtieFlag(
                                localPed,
                                kManagedFlags[flagIndex]) != FALSE;
                    }
                    localLassoFlagsCaptured_ = true;
                }
                for (const auto flag : kManagedFlags) {
                    // Flag 9 permits strong pull-over forces and is deliberately
                    // left at the user's original value; enabling it together
                    // with network root correction can launch both players.
                    if (flag == 9) {
                        continue;
                    }
                    const bool enable = flag != 8 && flag != 11;
                    SetPedLassoHogtieFlag(
                        localPed,
                        flag,
                        enable ? TRUE : FALSE);
                }
                SetPedCanBeLassoed(localPed, TRUE);
                if (WEAPON::HAS_PED_GOT_WEAPON(
                        *actor,
                        static_cast<Hash>(kWeaponLasso),
                        FALSE,
                        FALSE) == FALSE) {
                    WEAPON::GIVE_DELAYED_WEAPON_TO_PED(
                        *actor,
                        static_cast<Hash>(kWeaponLasso),
                        1,
                        TRUE,
                        0);
                }
                WEAPON::SET_CURRENT_PED_WEAPON(
                    *actor,
                    static_cast<Hash>(kWeaponLasso),
                    TRUE,
                    0,
                    FALSE,
                    FALSE);
                TaskLassoPed(
                    static_cast<Ped>(*actor),
                    localPed);
                PED::SET_PED_KEEP_TASK(*actor, TRUE);
                remotePeerLassoRopeActionId_ = action.actionId;
                remotePeerLassoTaskStartedMs_ = now;
                remotePeerLassoTaskAttempts_ = 1U;
                remotePeerLassoTaskPending_ = true;
                remotePeerLassoEngineOwned_ = false;
                ++remotePlayerActionLassoPending_;
                Log(
                    "[LASSO_ROPE] native catch task started after sender-confirmed physical effect for action-id=" +
                    std::to_string(action.actionId));
                Log(
                    "[LASSO_LIFECYCLE] direction=rx, state=throw-started, correlation=action-" +
                    std::to_string(action.actionId) +
                    ", action-id=" + std::to_string(action.actionId) +
                    ", reason=sender-confirmed-physical-effect");
            }
        }
        Log(
            "[ACTION_FSM] apply kind=" +
            std::to_string(static_cast<std::uint8_t>(action.kind)) +
            ", phase=" +
            std::to_string(static_cast<std::uint8_t>(action.phase)) +
            ", action-id=" + std::to_string(action.actionId) +
            ", revision=" + std::to_string(action.revision) +
            ", target-effect=" +
            std::to_string(appliesPhysicalTargetEffect ? 1 : 0));
        return true;
#else
        (void)action;
        return false;
#endif
    } catch (...) {
        Log("[ERROR][ACTION_APPLY] exception while applying player action");
        return false;
    }
}

bool ScriptHookSdkFacade::ApplyInteractionResult(
    const InteractionResultPayload& result,
    const NetEntityId localEntityId) noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        if (result.status != InteractionResultStatus::Completed) {
            return true;
        }
        const bool targetsLocal =
            result.targetEntityId == localEntityId;
        Entity target{};
        if (targetsLocal) {
            target = PLAYER::PLAYER_PED_ID();
        } else {
            target = replicas_.FindLocal(
                result.targetEntityId).value_or(0);
        }
        if (result.kind == InteractionKind::Revive ||
            result.kind == InteractionKind::EmergencyRecover) {
            if (target == 0 ||
                ENTITY::DOES_ENTITY_EXIST(target) == FALSE) {
                return false;
            }
            if (targetsLocal) {
                MaintainLocalDownedState(false, 0.35F);
            } else {
                AI::CLEAR_PED_TASKS_IMMEDIATELY(
                    target,
                    FALSE,
                    TRUE);
                const auto maximumHealth = std::max(
                    ENTITY::GET_ENTITY_MAX_HEALTH(target, FALSE),
                    1);
                ENTITY::SET_ENTITY_HEALTH(
                    target,
                    static_cast<int>(std::lround(
                        static_cast<float>(maximumHealth) * 0.35F)),
                    0);
            }
            Log(
                "[DOWNED_FSM] authoritative revive/recovery completed for entity " +
                std::to_string(result.targetEntityId.Value()));
            return true;
        }
        if (result.kind == InteractionKind::DismountPeer ||
            result.kind == InteractionKind::DismountSelf) {
            if (target == 0 ||
                ENTITY::DOES_ENTITY_EXIST(target) == FALSE) {
                return false;
            }
            if (PED::IS_PED_ON_MOUNT(target) != FALSE) {
                TaskDismountAnimal(static_cast<Ped>(target));
            }
            return true;
        }
        if (result.kind == InteractionKind::MountDriver ||
            result.kind == InteractionKind::MountPassenger) {
            if (!result.secondaryEntityId.IsValid()) {
                return false;
            }
            // A shared vehicle is resolved only through bridge-owned local
            // registries.  The wire identity is never treated as an RDR2
            // handle, which prevents a peer from seating a ped in an
            // arbitrary local carriage.
            auto vehicleHandle = remoteVehicleReplicas_.FindLocal(
                result.secondaryEntityId);
            if (!vehicleHandle.has_value() && localKnownVehicleHandle_ != 0) {
                vehicleHandle = localKnownVehicleHandle_;
            }
            if (!vehicleHandle.has_value() ||
                ENTITY::DOES_ENTITY_EXIST(*vehicleHandle) == FALSE) {
                // Horse relationships remain applied by the next
                // PlayerMountState, as before.
                return true;
            }
            const auto rider = result.actorEntityId == localEntityId
                ? PLAYER::PLAYER_PED_ID()
                : replicas_.FindLocal(result.actorEntityId).value_or(0);
            if (rider == 0 || rider == PLAYER::PLAYER_PED_ID() &&
                result.actorEntityId != localEntityId) {
                return false;
            }
            const auto seat = result.kind == InteractionKind::MountDriver
                ? -1
                : 0;
            const auto vehicle = static_cast<Vehicle>(*vehicleHandle);
            if (VEHICLE::GET_PED_IN_VEHICLE_SEAT(vehicle, seat) != rider) {
                PED::SET_PED_INTO_VEHICLE(rider, vehicle, seat);
            }
            Log("[SHARED_WAGON] authoritative seat assignment applied");
            return true;
        }
        return true;
#else
        (void)result;
        (void)localEntityId;
        return false;
#endif
    } catch (...) {
        return false;
    }
}

bool ScriptHookSdkFacade::ApplyRestraintState(
    const RestraintStatePayload& state,
    const NetEntityId localEntityId) noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        if (state.subjectEntityId != localEntityId &&
            authoritativeRemoteRestraintRevision_ != 0U &&
            state.revision != authoritativeRemoteRestraintRevision_ &&
            static_cast<std::int32_t>(
                state.revision - authoritativeRemoteRestraintRevision_) <= 0) {
            return true;
        }
        if (state.subjectEntityId == localEntityId &&
            authoritativeLocalRestraintRevision_ != 0U &&
            state.revision != authoritativeLocalRestraintRevision_ &&
            static_cast<std::int32_t>(
                state.revision - authoritativeLocalRestraintRevision_) <= 0) {
            return true;
        }
        if (state.state == PlayerRestraintState::Free) {
            // A two-player session can own only one peer rope transaction.
            // Release it on both endpoints: the victim owns its ragdoll while
            // the attacker may still own the visible rope/task locally.
            DeleteRemotePeerLassoRope(
                "authoritative-restraint-free");
            if (state.subjectEntityId != localEntityId) {
                authoritativeRemoteRestraintRevision_ = state.revision;
                authoritativeRemoteRestraint_ =
                    PlayerRestraintState::Free;
                authoritativeRemoteRestraintSubject_ = NetEntityId{};
                authoritativeRemoteRestraintReceivedMs_ = 0U;
                previousAuthoritativeRemoteRestraintRagdollMs_ = 0U;
                const auto remoteSubject = replicas_.FindLocal(
                    state.subjectEntityId).value_or(0);
                if (remoteSubject != 0 &&
                    ENTITY::DOES_ENTITY_EXIST(remoteSubject) != FALSE) {
                    AI::CLEAR_PED_TASKS_IMMEDIATELY(
                        remoteSubject,
                        FALSE,
                        TRUE);
                }
                Log(
                    "[RESTRAINT_FSM] remote restraint released by host authority");
                return true;
            }
        }
        if (state.subjectEntityId != localEntityId) {
            // Do not manufacture a second physical rope or a synthetic
            // ragdoll. On the attacker's PC the local engine already owns the
            // rope attached to this proxy. The replicated state only tells
            // the transform motor to yield until Free arrives.
            authoritativeRemoteRestraintRevision_ = state.revision;
            authoritativeRemoteRestraint_ = state.state;
            authoritativeRemoteRestraintSubject_ =
                state.subjectEntityId;
            authoritativeRemoteRestraintReceivedMs_ = TickMilliseconds();
            previousAuthoritativeRemoteRestraintRagdollMs_ = 0U;
            Log(
                state.state == PlayerRestraintState::Hogtied
                    ? "[RESTRAINT_FSM] authoritative remote hogtie latched; native task owns presentation"
                    : "[RESTRAINT_FSM] authoritative remote lasso latched; native rope owns presentation");
            return true;
        }
        const bool localRestraintChanged =
            authoritativeLocalRestraintRevision_ != state.revision ||
            authoritativeLocalRestraint_ != state.state;
        authoritativeLocalRestraintRevision_ = state.revision;
        authoritativeLocalRestraint_ = state.state;
        authoritativeLocalRestraintReceivedMs_ =
            state.state == PlayerRestraintState::Free
                ? 0U
                : TickMilliseconds();
        previousAuthoritativeRestraintRagdollMs_ = 0U;
        if (state.state == PlayerRestraintState::Free) {
            const auto localPed = PLAYER::PLAYER_PED_ID();
            if (localPed != 0 &&
                ENTITY::DOES_ENTITY_EXIST(localPed) != FALSE &&
                !localDownedPolicyActive_) {
                AI::CLEAR_PED_TASKS_IMMEDIATELY(
                    localPed,
                    FALSE,
                    TRUE);
            }
            Log("[RESTRAINT_FSM] local restraint released by host authority");
            return true;
        }
        const auto localPed = PLAYER::PLAYER_PED_ID();
        if (localPed == 0 ||
            ENTITY::DOES_ENTITY_EXIST(localPed) == FALSE) {
            return false;
        }
        const bool nativeConstraintActive =
            IsPedLassoed(localPed) != FALSE ||
            IsPedBeingHogtied(localPed) != FALSE ||
            IsPedHogtied(localPed) != FALSE;
        if (localRestraintChanged && !nativeConstraintActive &&
            PED::SET_PED_TO_RAGDOLL(
                localPed,
                kAuthoritativeRestraintRagdollDurationMilliseconds,
                kAuthoritativeRestraintRagdollDurationMilliseconds,
                0,
                TRUE,
                TRUE,
                FALSE) != FALSE) {
            previousAuthoritativeRestraintRagdollMs_ =
                TickMilliseconds();
            Log(
                "[VICTIM_CONSTRAINT] sender-confirmed lasso catch applied to the victim only; native rope was not yet visible locally");
        }
        Log(
            state.state == PlayerRestraintState::Hogtied
                ? "[RESTRAINT_FSM] authoritative local hogtie latched; native task owns presentation"
                : "[RESTRAINT_FSM] authoritative local lasso latched; awaiting/keeping native rope constraint");
        return true;
#else
        (void)state;
        (void)localEntityId;
        return false;
#endif
    } catch (...) {
        return false;
    }
}

void ScriptHookSdkFacade::MaintainLocalDownedState(
    const bool active,
    const float restoredHealthFraction) noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        const auto localPed = PLAYER::PLAYER_PED_ID();
        if (localPed == 0 ||
            ENTITY::DOES_ENTITY_EXIST(localPed) == FALSE) {
            return;
        }
        const auto localPlayer = PLAYER::PLAYER_ID();
        const auto now = TickMilliseconds();
        if (active) {
            if (!localDownedPolicyActive_) {
                localDownedPolicyActive_ = true;
                previousLocalDownedRagdollMs_ = 0U;
                Log(
                    "[DOWNED_FSM] local lethal guard entered; health authority held at incapacitated floor");
            }
            const auto maximumHealth = std::max(
                ENTITY::GET_ENTITY_MAX_HEALTH(localPed, FALSE),
                1);
            const auto incapacitatedHealth = std::max(
                1,
                static_cast<int>(std::lround(
                    static_cast<float>(maximumHealth) * 0.35F)));
            if (ENTITY::GET_ENTITY_HEALTH(localPed) <
                incapacitatedHealth) {
                ENTITY::SET_ENTITY_HEALTH(
                    localPed,
                    incapacitatedHealth,
                    0);
            }
            ENTITY::SET_ENTITY_INVINCIBLE(localPed, TRUE);
            ENTITY::SET_ENTITY_CAN_BE_DAMAGED(localPed, FALSE);
            PLAYER::SET_EVERYONE_IGNORE_PLAYER(localPlayer, TRUE);
            if (previousLocalDownedRagdollMs_ == 0U ||
                now < previousLocalDownedRagdollMs_ ||
                now - previousLocalDownedRagdollMs_ >= 700U) {
                (void)PED::SET_PED_TO_RAGDOLL(
                    localPed,
                    1'250,
                    1'250,
                    0,
                    TRUE,
                    TRUE,
                    FALSE);
                previousLocalDownedRagdollMs_ = now;
            }
            return;
        }

        if (!localDownedPolicyActive_) {
            return;
        }
        localDownedPolicyActive_ = false;
        previousLocalDownedRagdollMs_ = 0U;
        ENTITY::SET_ENTITY_INVINCIBLE(localPed, FALSE);
        ENTITY::SET_ENTITY_CAN_BE_DAMAGED(localPed, TRUE);
        PLAYER::SET_EVERYONE_IGNORE_PLAYER(localPlayer, FALSE);
        const auto maximumHealth = std::max(
            ENTITY::GET_ENTITY_MAX_HEALTH(localPed, FALSE),
            1);
        const auto restoreFraction = std::clamp(
            std::isfinite(restoredHealthFraction) &&
                    restoredHealthFraction > 0.0F
                ? restoredHealthFraction
                : 0.35F,
            0.01F,
            1.0F);
        ENTITY::SET_ENTITY_HEALTH(
            localPed,
            std::max(
                1,
                static_cast<int>(std::lround(
                    static_cast<float>(maximumHealth) *
                    restoreFraction))),
            0);
        AI::CLEAR_PED_TASKS_IMMEDIATELY(
            localPed,
            FALSE,
            TRUE);
        Log(
            "[DOWNED_FSM] local player restored with authoritative health fraction " +
            std::to_string(restoreFraction));
#else
        (void)active;
        (void)restoredHealthFraction;
#endif
    } catch (...) {
    }
}

void ScriptHookSdkFacade::ConfigureMotionReplication(
    const MotionReplicationConfigPayload& config) noexcept {
    try {
        if (config.revision == 0U ||
            (config.revision <= motionReplicationRevision_ &&
             config.mode == motionReplicationMode_ &&
             config.flags == motionReplicationFlags_)) {
            return;
        }
        const auto previousMode = motionReplicationMode_;
        const bool storyVmProbeEnabled =
            (config.flags & static_cast<std::uint16_t>(
                                MotionReplicationConfigFlag::
                                    EnableAnimSceneStoryVmProbe)) != 0U;
        if (storyVmProbeEnabled != animSceneHybridInspectorEnabled_) {
            animSceneHybridInspectorEnabled_ = storyVmProbeEnabled;
            animSceneHybridInspectorAttempted_ = false;
            Log(
                storyVmProbeEnabled
                    ? "[ANIMSCENE_HYBRID][CAPTURE][CONFIG] enabled experimental exact-build Story VM capture"
                    : "[ANIMSCENE_HYBRID][CAPTURE][CONFIG] disabled; detours removed and SAFE_FALLBACK selected");
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
            if (!storyVmProbeEnabled) {
                RemoveAnimSceneCaptureHooks();
                animSceneHybridHandlersValidated_ = false;
                animSceneHybridResolvedHandlers_.fill(nullptr);
                animSceneHybridNativeCreationEnabled_ = false;
            }
#endif
        }
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        if (storyVmProbeEnabled && !animSceneHybridInspectorAttempted_) {
            InspectAnimSceneHybridHandlers(0);
        }
#endif
        motionReplicationRevision_ = config.revision;
        motionReplicationMode_ = config.mode;
        motionReplicationFlags_ = config.flags;
        latestRemoteAnimationState_.reset();
        latestRemoteAnimationStateReceivedAtMs_.reset();
        animGraphReplicaPreparedEntity_ = {};
        lastRemoteAnimationStateHash_ = 0U;
        lastRemoteAnimationGraphHash_ = 0U;
        animGraphReplicaTicks_ = 0U;
        animGraphReplicaCorrections_ = 0U;
        animGraphReplicaStateApplies_ = 0U;
        animGraphReplicaMoveNetworkSamples_ = 0U;
        animGraphReplicaUnavailableSamples_ = 0U;
        animGraphReplicaDiagnosticsStartedMs_ = 0U;
        animGraphReplicaPositionErrorSum_ = 0.0;
        animGraphReplicaPositionErrorMax_ = 0.0F;
        ResetRemoteMotionTracking();

#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        if (remotePlayerId_.IsValid()) {
            const auto handle = replicas_.FindLocal(remotePlayerId_);
            if (handle.has_value() &&
                ENTITY::DOES_ENTITY_EXIST(*handle) != FALSE &&
                PED::IS_PED_RAGDOLL(*handle) == FALSE &&
                PED::IS_PED_FALLING(*handle) == FALSE &&
                PED::IS_PED_JUMPING(*handle) == FALSE &&
                PED::IS_PED_CLIMBING(*handle) == FALSE &&
                AI::IS_PED_GETTING_UP(*handle) == FALSE) {
                // Release the controller owned by the previous mode once. The
                // AnimGraph/Direct path never starts a navmesh locomotion task;
                // Task/Navmesh will rebuild its task on its next tick.
                AI::CLEAR_PED_TASKS(*handle, TRUE, TRUE);
            }
        }
#endif
        Log(
            motionReplicationMode_ ==
                    MotionReplicationWireMode::AnimGraphReplica
                ? "[INFO][ANIMGRAPH_REPLICA] enabled: direct network root; Task/Navmesh motor disabled"
                : "[INFO][ANIMGRAPH_REPLICA] disabled: Task/Navmesh puppet active");
        if (previousMode != motionReplicationMode_ &&
            (config.flags & static_cast<std::uint16_t>(
                 MotionReplicationConfigFlag::AllowTaskNavmeshFallback)) !=
                0U) {
            Log(
                "[WARN][ANIMGRAPH_REPLICA] fallback flag received; this build keeps engines separate and does not blend controllers");
        }
    } catch (...) {
        Log("[ERROR][ANIMGRAPH_REPLICA] failed to apply motion configuration");
    }
}

bool ScriptHookSdkFacade::ApplyRemoteAnimationState(
    const PlayerAnimationStatePayload& state) noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        if (motionReplicationMode_ !=
                MotionReplicationWireMode::AnimGraphReplica ||
            !state.entityId.IsValid() ||
            (remotePlayerId_.IsValid() &&
             remotePlayerId_ != state.entityId)) {
            return false;
        }

        const bool newSample =
            !latestRemoteAnimationState_.has_value() ||
            latestRemoteAnimationState_->entityId != state.entityId ||
            latestRemoteAnimationState_->locomotionEpoch !=
                state.locomotionEpoch ||
            latestRemoteAnimationState_->sampleSequence !=
                state.sampleSequence;
        latestRemoteAnimationState_ = state;
        if (newSample) {
            latestRemoteAnimationStateReceivedAtMs_ = TickMilliseconds();
        }
        const auto handle = replicas_.FindLocal(state.entityId);
        if (!handle.has_value() ||
            ENTITY::DOES_ENTITY_EXIST(*handle) == FALSE) {
            // The state is still useful: BridgeRuntime can deliver it again
            // immediately after the player proxy is recreated.
            return true;
        }
        if (IsOwnedHybridAnimSceneEntity(*handle)) {
            // The native AnimScene owns the complete actor graph, phase and IK.
            // Applying the generic player AnimGraph on top would cut authored
            // clips and was one source of T-pose/teleport oscillation.
            return true;
        }

        constexpr auto kStateHashValid = static_cast<std::uint32_t>(
            PlayerAnimationStateFlag::StateHashValid);
        constexpr auto kGraphHashValid = static_cast<std::uint32_t>(
            PlayerAnimationStateFlag::GraphHashValid);
        const auto reportedGraph =
            (state.flags & kGraphHashValid) != 0U
                ? state.graphHash
                : 0U;
        const auto reportedState =
            (state.flags & kStateHashValid) != 0U
                ? state.stateHash
                : 0U;
        if (reportedGraph != lastRemoteAnimationGraphHash_ ||
            reportedState != lastRemoteAnimationStateHash_) {
            lastRemoteAnimationGraphHash_ = reportedGraph;
            lastRemoteAnimationStateHash_ = reportedState;
            Log(
                "[INFO][ANIMGRAPH_SAMPLE] source=" +
                std::to_string(
                    static_cast<std::uint8_t>(state.source)) +
                ", graph=" + std::to_string(reportedGraph) +
                ", state=" + std::to_string(reportedState) +
                ", capabilities=" +
                std::to_string(state.capabilities));
        }
        // Applying the state is owned by ApplyRemoteAnimGraphTransform, where
        // mounted/ragdoll/lasso/fall protection is known. Doing it here would
        // force a motion state before that guard on every received packet.
        return true;
#else
        (void)state;
        return false;
#endif
    } catch (...) {
        ++animGraphReplicaUnavailableSamples_;
        return false;
    }
}

bool ScriptHookSdkFacade::ApplyRemoteAnimGraphTransform(
    const LocalEntityHandle handle,
    const PlayerStatePayload& state) noexcept {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
    if (handle == 0 || ENTITY::DOES_ENTITY_EXIST(handle) == FALSE) {
        return false;
    }

    const auto now = TickMilliseconds();
    RestoreRemoteVisualFireAmmo(handle, now, false);
    const bool visualFireGraphOwnsTask =
        remoteVisualFireSuppressedWeaponHash_ != 0U &&
        remoteVisualFireRestoreAtMs_ != 0U &&
        now < remoteVisualFireRestoreAtMs_;
    const auto current = ToBridgeVector(
        ENTITY::GET_ENTITY_COORDS(handle, TRUE, FALSE));
    if (!IsFinite(current)) {
        return false;
    }
    const auto positionError = Distance(current, state.position);
    if (!std::isfinite(positionError)) {
        return false;
    }

    const auto localPed = PLAYER::PLAYER_PED_ID();
    const bool physicalLassoOwnsRemoteProxy =
        localPed != 0 &&
        ENTITY::DOES_ENTITY_EXIST(localPed) != FALSE &&
        (GetLassoTarget(localPed) == static_cast<Ped>(handle) ||
         IsPedLassoed(static_cast<Ped>(handle)) != FALSE ||
         IsPedBeingHogtied(static_cast<Ped>(handle)) != FALSE ||
         IsPedHogtied(static_cast<Ped>(handle)) != FALSE);
    if (physicalLassoOwnsRemoteProxy) {
        // On the thrower's machine the engine constraint must own both ends
        // of the rope. Pulling the caught proxy back to its network root made
        // the constraint topple the local thrower instead of keeping the
        // victim attached.
        ENTITY::FORCE_ENTITY_AI_AND_ANIMATION_UPDATE(handle, FALSE);
        ++animGraphPhysicalRootYieldTicks_;
        return true;
    }

    if (reliablePlayerActionProtocolObserved_ &&
        remoteActionLassoActive_ &&
        remotePeerLassoRopeActionId_ != 0U &&
        (remotePeerLassoTaskPending_ ||
         remotePeerLassoEngineOwned_)) {
        // TASK_LASSO_PED owns the actor's locomotion and upper-body graph.
        // Any straight-to/aim/root task issued here would replace the rope
        // task and produce the old thrown-weapon/stacked-lasso artifact.
        ENTITY::FORCE_ENTITY_AI_AND_ANIMATION_UPDATE(handle, FALSE);
        ++remotePlayerActionMotorYields_;
        return true;
    }

    constexpr auto kMounted = static_cast<std::uint32_t>(
        PlayerStateFlag::Mounted);
    const bool mounted =
        (state.flags & kMounted) != 0U &&
        remotePlayerMounted_;
    const bool localPhysicalBefore =
        PED::IS_PED_RAGDOLL(handle) != FALSE ||
        PED::IS_PED_FALLING(handle) != FALSE ||
        PED::IS_PED_JUMPING(handle) != FALSE ||
        PED::IS_PED_CLIMBING(handle) != FALSE ||
        AI::IS_PED_GETTING_UP(handle) != FALSE;

    for (auto iterator = pendingRemoteTraversals_.begin();
         iterator != pendingRemoteTraversals_.end();) {
        const auto age =
            now >= iterator->capturedAtMs
                ? now - iterator->capturedAtMs
                : 0U;
        if (age > kDirectReplicaTraversalMaximumAgeMs) {
            iterator = pendingRemoteTraversals_.erase(iterator);
            ++animGraphTraversalExpired_;
        } else {
            ++iterator;
        }
    }

    bool traversalStarted{};
    if (!pendingRemoteTraversals_.empty()) {
        for (auto iterator = pendingRemoteTraversals_.begin();
             iterator != pendingRemoteTraversals_.end();
             ++iterator) {
            const auto age =
                now >= iterator->capturedAtMs
                    ? now - iterator->capturedAtMs
                    : 0U;
            const bool senderActionActive =
                state.traversalActionId == iterator->actionId &&
                state.traversalKind != PlayerTraversalKind::None;
            if (!ShouldStartDirectReplicaTraversal(
                    DirectReplicaTraversalStartInput{
                        age,
                        HorizontalDistance(current, iterator->position),
                        senderActionActive,
                        localPhysicalBefore,
                        mounted,
                        now < remoteTraversalTaskGuardUntilMs_})) {
                continue;
            }

            ENTITY::SET_ENTITY_HEADING(handle, iterator->heading);
            if (iterator->kind == RemoteTraversalKind::Climb) {
                AI::TASK_CLIMB(handle, TRUE);
                ++animGraphTraversalClimbTaskStarts_;
            } else {
                AI::TASK_JUMP(handle, TRUE);
                ++animGraphTraversalJumpTaskStarts_;
            }
            PED::SET_PED_KEEP_TASK(handle, TRUE);
            const auto traversalGuardMs =
                iterator->kind == RemoteTraversalKind::Climb
                    ? kRemoteTraversalClimbTaskGuardMs
                    : kRemoteTraversalJumpTaskGuardMs;
            remoteTraversalTaskGuardUntilMs_ =
                now + traversalGuardMs;
            const auto startedActionId = iterator->actionId;
            const bool startedClimb =
                iterator->kind == RemoteTraversalKind::Climb;
            pendingRemoteTraversals_.erase(iterator);
            animGraphVisualTaskActive_ = false;
            animGraphVisualTaskStartedMs_ = 0U;
            traversalStarted = true;
            Log(
                "[INFO][ANIMGRAPH_TRAVERSAL] native task started: id=" +
                std::to_string(startedActionId) +
                ", kind=" +
                (startedClimb ? "climb" : "jump") +
                ", direct-root-yield-ms=" +
                std::to_string(traversalGuardMs));
            break;
        }
    }

    const auto previousSemanticMode =
        animGraphLocomotionModeInitialized_
            ? animGraphPreviousLocomotionMode_
            : PlayerLocomotionMode::Grounded;
    const bool semanticRagdollRequested =
        !mounted &&
        state.locomotionMode == PlayerLocomotionMode::Ragdoll;
    if (!semanticRagdollRequested) {
        previousRemoteSemanticRagdollMs_ = 0U;
    }
    const bool semanticRagdollRefreshDue =
        semanticRagdollRequested &&
        (!localPhysicalBefore ||
         previousRemoteSemanticRagdollMs_ == 0U ||
         now < previousRemoteSemanticRagdollMs_ ||
         now - previousRemoteSemanticRagdollMs_ >= 700U);
    const bool ragdollStarted =
        semanticRagdollRefreshDue &&
        PED::SET_PED_TO_RAGDOLL(
            handle,
            1'250,
            1'250,
            0,
            TRUE,
            TRUE,
            FALSE) != FALSE;
    if (ragdollStarted) {
        previousRemoteSemanticRagdollMs_ = now;
        animGraphVisualTaskActive_ = false;
        animGraphVisualTaskStartedMs_ = 0U;
        ++animGraphRagdollTaskStarts_;
    }
    const bool airborneLaunched =
        ShouldLaunchDirectReplicaAirborne(
            previousSemanticMode,
            state.locomotionMode,
            localPhysicalBefore || traversalStarted || ragdollStarted,
            mounted);
    if (airborneLaunched) {
        // A plain ledge fall has no PlayerTraversal transaction. Seed RDR2's
        // native physics once from the authoritative point/velocity so its
        // own falling graph can take over. If local physics does not engage,
        // direct-root correction resumes on the next frame.
        ENTITY::SET_ENTITY_COORDS_NO_OFFSET(
            handle,
            state.position.x,
            state.position.y,
            state.position.z,
            TRUE,
            TRUE,
            FALSE);
        ENTITY::SET_ENTITY_HEADING(handle, state.heading);
        ENTITY::SET_ENTITY_VELOCITY(
            handle,
            state.velocity.x,
            state.velocity.y,
            state.velocity.z);
        animGraphVisualTaskActive_ = false;
        animGraphVisualTaskStartedMs_ = 0U;
        ++animGraphAirborneLaunches_;
    }
    animGraphPreviousLocomotionMode_ = state.locomotionMode;
    animGraphLocomotionModeInitialized_ = true;

    const bool observedPhysicalAnimation =
        PED::IS_PED_FALLING(handle) != FALSE ||
        PED::IS_PED_JUMPING(handle) != FALSE ||
        PED::IS_PED_CLIMBING(handle) != FALSE;
    const bool physicalAnimation =
        PED::IS_PED_RAGDOLL(handle) != FALSE ||
        AI::IS_PED_GETTING_UP(handle) != FALSE ||
        observedPhysicalAnimation || traversalStarted ||
        airborneLaunched || ragdollStarted ||
        now < remoteTraversalTaskGuardUntilMs_ ||
        state.locomotionMode == PlayerLocomotionMode::Ragdoll;
    const bool expectedTraversalAnimation =
        state.locomotionMode == PlayerLocomotionMode::Traversal ||
        state.locomotionMode == PlayerLocomotionMode::Airborne;
    if (expectedTraversalAnimation) {
        ++animGraphTraversalExpectedTicks_;
        if (observedPhysicalAnimation) {
            ++animGraphTraversalObservedTicks_;
        } else {
            ++animGraphTraversalMissingTicks_;
        }
    }
    const bool applyVisualRoot =
        ShouldApplyAnimGraphDirectRootCorrection(
            mounted,
            physicalAnimation);
    const bool applyPhysicalRootLeash =
        ShouldApplyDirectReplicaPhysicalRootLeash(
            mounted,
            physicalAnimation,
            positionError);
    const bool applyCoordinateRoot =
        applyVisualRoot || applyPhysicalRootLeash;
    if (!mounted && !applyVisualRoot) {
        ++animGraphPhysicalRootYieldTicks_;
    }
    if (applyPhysicalRootLeash) {
        ++animGraphPhysicalRootLeashCorrections_;
    }
    if (applyVisualRoot &&
        animGraphReplicaPreparedEntity_ != state.entityId) {
        AI::CLEAR_PED_TASKS(handle, TRUE, TRUE);
        AI::TASK_SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(handle, TRUE);
        PED::SET_PED_KEEP_TASK(handle, FALSE);
        PED::SET_PED_CAN_PLAY_AMBIENT_ANIMS(handle, TRUE);
        PED::SET_PED_CAN_PLAY_AMBIENT_BASE_ANIMS(handle, TRUE);
        // Allow RDR2's own locomotion/weapon graph to solve feet, hands,
        // torso and look pose after each authoritative root correction.
        // These switches do not fabricate exact sender IK targets; they keep
        // the native ground and weapon solvers available on the replica.
        PED::SET_PED_CAN_ARM_IK(handle, TRUE);
        PED::SET_PED_CAN_HEAD_IK(handle, TRUE);
        PED::SET_PED_CAN_LEG_IK(handle, TRUE);
        PED::SET_PED_CAN_TORSO_IK(handle, TRUE);
        ++animGraphIkPreparations_;
        animGraphReplicaPreparedEntity_ = state.entityId;
        animGraphVisualTaskActive_ = false;
        animGraphVisualTaskStartedMs_ = 0U;
        Log(
            "[INFO][ANIMGRAPH_REPLICA] remote ped prepared; direct root with native visual locomotion driver, navmesh disabled");
    } else if (!applyVisualRoot) {
        // Never replace a ragdoll, traversal, lasso, fall or mount task. Mark
        // the visual driver stale so a fresh gait task is issued only after
        // the protected physical animation yields ownership.
        animGraphVisualTaskActive_ = false;
        animGraphVisualTaskStartedMs_ = 0U;
    }
    if (applyCoordinateRoot &&
        positionError >= kAnimGraphReplicaCoordinateDeadZoneMeters) {
        // The three legacy flags map to keepTasks, keepIK and doWarp on this
        // native hash. Clearing the first two on every network correction
        // repeatedly destroyed the locomotion graph and caused the observed
        // 99% T-pose/flicker even though FORCE_PED_MOTION_STATE returned true.
        ENTITY::SET_ENTITY_COORDS_NO_OFFSET(
            handle,
            state.position.x,
            state.position.y,
            state.position.z,
            TRUE,
            TRUE,
            FALSE);
        ++animGraphReplicaCorrections_;
    }
    const auto currentVisualHeading =
        ENTITY::GET_ENTITY_HEADING(handle);
    const auto visualHeadingError =
        AbsoluteHeadingDifference(currentVisualHeading, state.heading);
    const auto idleTurnBlockingFlags =
        static_cast<std::uint32_t>(PlayerStateFlag::Aiming) |
        static_cast<std::uint32_t>(PlayerStateFlag::MeleeCombat) |
        static_cast<std::uint32_t>(PlayerStateFlag::MeleeBlocking) |
        static_cast<std::uint32_t>(PlayerStateFlag::MeleeGrappling) |
        static_cast<std::uint32_t>(PlayerStateFlag::InCover) |
        static_cast<std::uint32_t>(PlayerStateFlag::GoingIntoCover) |
        static_cast<std::uint32_t>(PlayerStateFlag::InWater) |
        static_cast<std::uint32_t>(PlayerStateFlag::Swimming) |
        static_cast<std::uint32_t>(PlayerStateFlag::SwimmingUnderwater);
    const bool nativeIdleTurnRequested =
        applyVisualRoot &&
        state.locomotionMode == PlayerLocomotionMode::Grounded &&
        state.desiredMoveBlend < 0.10F &&
        std::hypot(state.velocity.x, state.velocity.y) < 0.20F &&
        (state.flags & idleTurnBlockingFlags) == 0U &&
        visualHeadingError >=
            kDirectReplicaTurnInPlaceHeadingDegrees;
    if (applyVisualRoot && !nativeIdleTurnRequested &&
        visualHeadingError >=
            kAnimGraphReplicaHeadingDeadZoneDegrees) {
        ENTITY::SET_ENTITY_HEADING(handle, state.heading);
    }

    constexpr auto kStealthMovement = static_cast<std::uint32_t>(
        PlayerStateFlag::StealthMovement);
    constexpr auto kInCover = static_cast<std::uint32_t>(
        PlayerStateFlag::InCover);
    constexpr auto kGoingIntoCover = static_cast<std::uint32_t>(
        PlayerStateFlag::GoingIntoCover);
    constexpr auto kCoverFacingLeft = static_cast<std::uint32_t>(
        PlayerStateFlag::CoverFacingLeft);
    constexpr auto kAimingFromCover = static_cast<std::uint32_t>(
        PlayerStateFlag::AimingFromCover);
    const bool requestedCover =
        (state.flags & (kInCover | kGoingIntoCover)) != 0U &&
        !mounted && !physicalAnimation;
    const bool requestedStealth =
        (state.flags & kStealthMovement) != 0U &&
        !mounted && !physicalAnimation && !requestedCover;
    const bool requestedCoverFacingLeft =
        (state.flags & kCoverFacingLeft) != 0U;
    const bool requestedAimingFromCover =
        (state.flags & kAimingFromCover) != 0U;
    const bool coverStateChanged =
        requestedCover != animGraphCoverActive_ ||
        requestedCoverFacingLeft != animGraphCoverFacingLeft_ ||
        requestedAimingFromCover != animGraphAimingFromCover_;
    const bool enteringCover =
        requestedCover && !animGraphCoverActive_;
    const bool nativeInCover =
        requestedCover &&
        PED::IS_PED_IN_COVER(handle, FALSE, FALSE) != FALSE;
    if (requestedCover) {
        ++animGraphCoverExpectedTicks_;
        if (nativeInCover) {
            ++animGraphCoverObservedTicks_;
            animGraphCoverMissingSinceMs_ = 0U;
            animGraphCoverAcquireStartedMs_ = 0U;
            animGraphCoverAcquireRetries_ = 0U;
        } else if (animGraphCoverMissingSinceMs_ == 0U) {
            animGraphCoverMissingSinceMs_ = now;
        }
    } else {
        animGraphCoverMissingSinceMs_ = 0U;
    }
    const bool coverHoldRefreshDue =
        nativeInCover &&
        (previousRemoteCoverTaskMs_ == 0U ||
         now < previousRemoteCoverTaskMs_ ||
         now - previousRemoteCoverTaskMs_ >=
             kRemoteCoverHoldRefreshMilliseconds);
    const bool coverAcquireRetryDue =
        requestedCover && !nativeInCover && !enteringCover &&
        animGraphCoverAcquireStartedMs_ != 0U &&
        now >= animGraphCoverAcquireStartedMs_ &&
        now - animGraphCoverAcquireStartedMs_ >=
            kRemoteCoverAcquireRetryMilliseconds &&
        animGraphCoverAcquireRetries_ <
            kRemoteCoverMaximumAcquireRetries;
    const bool coverRecoveryDue =
        requestedCover && !nativeInCover && !enteringCover &&
        !coverAcquireRetryDue &&
        animGraphCoverMissingSinceMs_ != 0U &&
        now >= animGraphCoverMissingSinceMs_ &&
        now - animGraphCoverMissingSinceMs_ >=
            kRemoteCoverLostRecoveryMilliseconds &&
        (animGraphPreviousCoverRecoveryMs_ == 0U ||
         now < animGraphPreviousCoverRecoveryMs_ ||
         now - animGraphPreviousCoverRecoveryMs_ >=
             kRemoteCoverRecoveryCooldownMilliseconds);
    if (requestedCover &&
        (enteringCover || coverHoldRefreshDue ||
         coverAcquireRetryDue || coverRecoveryDue)) {
        if (coverHoldRefreshDue && !enteringCover) {
            AI::TASK_STAY_IN_COVER(handle);
        } else {
            AI::TASK_PUT_PED_DIRECTLY_INTO_COVER(
                handle,
                state.position.x,
                state.position.y,
                state.position.z,
                1'500,
                FALSE,
                requestedCoverFacingLeft ? -1.0F : 1.0F,
                TRUE,
                FALSE,
                0,
                FALSE,
                FALSE,
                FALSE);
            if (enteringCover) {
                animGraphCoverAcquireStartedMs_ = now;
                animGraphCoverAcquireRetries_ = 0U;
            } else if (coverRecoveryDue) {
                animGraphCoverAcquireStartedMs_ = now;
                animGraphCoverAcquireRetries_ =
                    kRemoteCoverMaximumAcquireRetries;
                animGraphPreviousCoverRecoveryMs_ = now;
                ++animGraphCoverReacquires_;
                Log(
                    "[WARNING][COVER_RX] replicated cover graph stayed detached; native cover reacquire issued");
            } else {
                ++animGraphCoverAcquireRetries_;
                animGraphCoverAcquireStartedMs_ = now;
            }
        }
        PED::SET_PED_KEEP_TASK(handle, TRUE);
        previousRemoteCoverTaskMs_ = now;
        animGraphVisualTaskActive_ = false;
        animGraphVisualTaskStartedMs_ = 0U;
        ++animGraphCoverTaskStarts_;
    } else if (!requestedCover && animGraphCoverActive_) {
        AI::CLEAR_PED_TASKS(handle, TRUE, TRUE);
        previousRemoteCoverTaskMs_ = 0U;
        animGraphCoverAcquireStartedMs_ = 0U;
        animGraphCoverAcquireRetries_ = 0U;
        animGraphCoverMissingSinceMs_ = 0U;
        animGraphPreviousCoverRecoveryMs_ = 0U;
        animGraphVisualTaskActive_ = false;
        animGraphVisualTaskStartedMs_ = 0U;
        ++animGraphCoverTaskCancels_;
    }
    const bool coverFallbackDue =
        requestedCover && !nativeInCover &&
        animGraphCoverAcquireStartedMs_ != 0U &&
        now >= animGraphCoverAcquireStartedMs_ &&
        now - animGraphCoverAcquireStartedMs_ >=
            kRemoteCoverFallbackMilliseconds;
    if (coverFallbackDue &&
        !animGraphCoverFallbackCrouchActive_) {
        // Some Story interiors expose no usable local cover point even when
        // the sender is visibly attached to one. Preserve the semantic pose
        // with the native crouch graph instead of showing an upright idle
        // replica. The authoritative root/heading still comes from the
        // sender, so this fallback cannot navigate or invent movement.
        SetPedCrouchMovement(handle, TRUE, FALSE);
        animGraphCoverFallbackCrouchActive_ = true;
        previousRemoteCoverFallbackAssertMs_ = now;
        ++animGraphCoverFallbackStarts_;
        Log(
            "[WARNING][COVER_RX] native cover point not acquired after 300ms; using crouched cover fallback");
    } else if (animGraphCoverFallbackCrouchActive_ &&
               !requestedCover) {
        SetPedCrouchMovement(handle, FALSE, FALSE);
        animGraphCoverFallbackCrouchActive_ = false;
        previousRemoteCoverFallbackAssertMs_ = 0U;
    } else if (animGraphCoverFallbackCrouchActive_ &&
               requestedCover && !nativeInCover &&
               GetPedCrouchMovement(handle) == FALSE &&
               (previousRemoteCoverFallbackAssertMs_ == 0U ||
                now < previousRemoteCoverFallbackAssertMs_ ||
                now - previousRemoteCoverFallbackAssertMs_ >=
                    kRemoteCoverFallbackRecoveryMilliseconds)) {
        // Aim/fire/Story tasks may clear the crouch branch underneath the
        // semantic fallback. Reassert it only after a measured loss; doing it
        // every frame would continually restart the locomotion graph.
        SetPedCrouchMovement(handle, TRUE, FALSE);
        previousRemoteCoverFallbackAssertMs_ = now;
        ++animGraphCoverFallbackRecoveries_;
    }
    if (coverStateChanged) {
        ++animGraphCoverTransitions_;
    }
    animGraphCoverActive_ = requestedCover;
    animGraphCoverFacingLeft_ = requestedCoverFacingLeft;
    animGraphAimingFromCover_ = requestedAimingFromCover;
    const bool stealthStateChanged =
        requestedStealth != animGraphStealthActive_;
    if (stealthStateChanged) {
        // _SET_PED_CROUCH_MOVEMENT is an AnimGraph transition, not a harmless
        // per-frame override. Re-applying FALSE every render tick resets the
        // locomotion/aim graph and leaves an otherwise fresh remote snapshot
        // standing still. Apply it exactly once when the replicated semantic
        // state changes; the normal visual motor owns subsequent frames.
        if (requestedStealth) {
            SetPedCrouchMovement(handle, TRUE, FALSE);
        } else if (!requestedCover) {
            SetPedCrouchMovement(handle, FALSE, FALSE);
        } else {
            // Cover takes ownership of the same crouch graph. Clearing the
            // previous stealth state here used to cancel a cover fallback in
            // the very frame it was entered. Remember that ownership so the
            // crouch branch is released only on a real cover exit.
            animGraphCoverFallbackCrouchActive_ = true;
            previousRemoteCoverFallbackAssertMs_ = now;
        }
        animGraphStealthActive_ = requestedStealth;
        ++animGraphStealthTransitions_;
    }
    if (requestedStealth && !mounted && !physicalAnimation) {
        ++animGraphStealthExpectedTicks_;
        const bool nativeStealthObserved =
            GetPedCrouchMovement(handle) != FALSE ||
            PED::GET_PED_STEALTH_MOVEMENT(handle) != FALSE;
        if (nativeStealthObserved) {
            ++animGraphStealthObservedTicks_;
            animGraphStealthMissingSinceMs_ = 0U;
        } else {
            if (animGraphStealthMissingSinceMs_ == 0U) {
                animGraphStealthMissingSinceMs_ = now;
            }
            const bool stealthRecoveryDue =
                animGraphStealthMissingSinceMs_ != 0U &&
                now >= animGraphStealthMissingSinceMs_ &&
                now - animGraphStealthMissingSinceMs_ >=
                    kRemoteCrouchMissingRecoveryMilliseconds &&
                (animGraphPreviousStealthRecoveryMs_ == 0U ||
                 now < animGraphPreviousStealthRecoveryMs_ ||
                 now - animGraphPreviousStealthRecoveryMs_ >=
                     kRemoteCrouchRecoveryCooldownMilliseconds);
            if (stealthRecoveryDue) {
                SetPedCrouchMovement(handle, TRUE, FALSE);
                animGraphPreviousStealthRecoveryMs_ = now;
                animGraphStealthMissingSinceMs_ = now;
                ++animGraphStealthRecoveries_;
            }
        }
    } else {
        animGraphStealthMissingSinceMs_ = 0U;
        if (!requestedStealth) {
            animGraphPreviousStealthRecoveryMs_ = 0U;
        }
    }

    constexpr auto kInWater = static_cast<std::uint32_t>(
        PlayerStateFlag::InWater);
    constexpr auto kSwimming = static_cast<std::uint32_t>(
        PlayerStateFlag::Swimming);
    constexpr auto kSwimmingUnderwater = static_cast<std::uint32_t>(
        PlayerStateFlag::SwimmingUnderwater);
    const bool requestedInWater =
        (state.flags & kInWater) != 0U;
    const bool requestedSwimming =
        (state.flags & (kSwimming | kSwimmingUnderwater)) != 0U;
    if (requestedInWater) {
        ++animGraphWaterExpectedTicks_;
        if (ENTITY::IS_ENTITY_IN_WATER(handle) != FALSE) {
            ++animGraphWaterObservedTicks_;
        }
    }
    if (requestedSwimming) {
        ++animGraphSwimmingExpectedTicks_;
        const bool nativeSwimming =
            PED::IS_PED_SWIMMING(handle) != FALSE;
        const bool nativeUnderwater =
            PED::IS_PED_SWIMMING_UNDER_WATER(handle) != FALSE;
        if (nativeSwimming || nativeUnderwater) {
            ++animGraphSwimmingObservedTicks_;
        }
    }

    constexpr auto kAiming = static_cast<std::uint32_t>(
        PlayerStateFlag::Aiming);
    constexpr auto kAimTargetValid = static_cast<std::uint32_t>(
        PlayerStateFlag::AimTargetValid);
    constexpr auto kMeleeCombat = static_cast<std::uint32_t>(
        PlayerStateFlag::MeleeCombat);
    constexpr auto kMeleeBlocking = static_cast<std::uint32_t>(
        PlayerStateFlag::MeleeBlocking);
    constexpr auto kMeleeGrappling = static_cast<std::uint32_t>(
        PlayerStateFlag::MeleeGrappling);
    const bool hasAimTarget =
        (state.flags & kAimTargetValid) != 0U &&
        IsFinite(state.aimTarget);
    const bool requestedBlocking =
        (reliablePlayerActionProtocolObserved_
             ? remoteActionBlockActive_
             : (state.flags & kMeleeBlocking) != 0U) &&
        !mounted && !physicalAnimation;
    const bool requestedMelee =
        (reliablePlayerActionProtocolObserved_
             ? remoteActionMeleeActive_ || remoteActionGrappleActive_
             : (state.flags & kMeleeCombat) != 0U ||
                   (state.flags & kMeleeGrappling) != 0U) &&
        !requestedBlocking &&
        hasAimTarget && !mounted && !physicalAnimation;
    const bool requestedAiming =
        (reliablePlayerActionProtocolObserved_
             ? remoteActionAimActive_
             : (state.flags & kAiming) != 0U) &&
        hasAimTarget && !requestedMelee && !requestedBlocking &&
        remoteWeaponHash_ != 0U && !mounted && !physicalAnimation;
    const bool coverAimRefreshDue =
        requestedCover && nativeInCover &&
        requestedAimingFromCover && requestedAiming &&
        (previousRemoteAimTaskMs_ == 0U ||
         now < previousRemoteAimTaskMs_ ||
         now - previousRemoteAimTaskMs_ >=
             kRemotePlayerAimTaskRefreshMilliseconds);
    if (coverAimRefreshDue) {
        AI::TASK_AIM_GUN_AT_COORD(
            handle,
            state.aimTarget.x,
            state.aimTarget.y,
            state.aimTarget.z,
            kRemotePlayerAimTaskDurationMilliseconds,
            FALSE,
            FALSE);
        PED::SET_PED_KEEP_TASK(handle, TRUE);
        previousRemoteAimTaskMs_ = now;
        previousRemoteAimTarget_ = state.aimTarget;
        animGraphVisualTaskActive_ = false;
        animGraphVisualTaskStartedMs_ = 0U;
        ++animGraphAimIdleTaskStarts_;
    }
    if (requestedAiming != remoteAiming_) {
        if (!requestedAiming) {
            AI::CLEAR_PED_SECONDARY_TASK(handle);
            ++animGraphAimTaskCancels_;
        }
        remoteAiming_ = requestedAiming;
        previousRemoteAimTaskMs_ = 0U;
        previousRemoteAimTarget_ = {};
        animGraphAimTaskDestination_ = {};
        animGraphVisualTaskActive_ = false;
        animGraphVisualTaskStartedMs_ = 0U;
        ++remoteActionAimTransitions_;
        Log(
            requestedAiming
                ? "[INFO][AIM_POSE] direct AnimGraph aim layer started"
                : "[INFO][AIM_POSE] direct AnimGraph aim layer ended; locomotion reacquired");
    }
    if (requestedBlocking != remoteMeleeBlocking_) {
        remoteMeleeBlocking_ = requestedBlocking;
        Log(
            requestedBlocking
                ? "[INFO][PEER_COMBAT] remote block intent started"
                : "[INFO][PEER_COMBAT] remote block intent released");
    }
    if (requestedMelee != remoteMeleeCombat_) {
        remoteMeleeCombat_ = requestedMelee;
        previousRemoteMeleeTaskMs_ = 0U;
        animGraphVisualTaskActive_ = false;
        animGraphVisualTaskStartedMs_ = 0U;
        Log(
            requestedMelee
                ? "[INFO][PEER_COMBAT] remote melee attack/grapple pulse started"
                : "[INFO][PEER_COMBAT] remote melee attack/grapple pulse released");
    }

    const bool meleeTaskOwnsGraph =
        remoteMeleeVisualActionId_ != 0U &&
        remoteMeleeVisualDeadlineMs_ != 0U &&
        now < remoteMeleeVisualDeadlineMs_;
    if (requestedMelee) {
        ++animGraphMeleeExpectedTicks_;
        if (!meleeTaskOwnsGraph) {
            ++animGraphMeleeMissingTargetTicks_;
        }
    }

    if (applyVisualRoot && !meleeTaskOwnsGraph &&
        !visualFireGraphOwnsTask && !requestedCover) {
        std::uint32_t motionState{};
        constexpr auto kStateHashValid = static_cast<std::uint32_t>(
            PlayerAnimationStateFlag::StateHashValid);
        const bool cachedAnimationFresh =
            latestRemoteAnimationStateReceivedAtMs_.has_value() &&
            IsRemoteAnimationStateFresh(
                *latestRemoteAnimationStateReceivedAtMs_,
                0U,
                0U,
                now);
        if (cachedAnimationFresh &&
            latestRemoteAnimationState_.has_value() &&
            latestRemoteAnimationState_->entityId == state.entityId &&
            latestRemoteAnimationState_->locomotionEpoch ==
                state.locomotionEpoch &&
            latestRemoteAnimationState_->source ==
                PlayerAnimationSampleSource::LocomotionNative &&
            (latestRemoteAnimationState_->flags & kStateHashValid) != 0U) {
            motionState = latestRemoteAnimationState_->stateHash;
        } else if (state.desiredMoveBlend < 0.10F) {
            motionState = kMotionStateIdle;
        } else if (state.desiredMoveBlend < 1.50F) {
            motionState = kMotionStateWalk;
        } else if (state.desiredMoveBlend < 2.50F) {
            motionState = kMotionStateRun;
        } else {
            motionState = kMotionStateSprint;
        }

        std::optional<RemoteLocomotion> reportedLocomotion{};
        if (motionState == kMotionStateIdle) {
            reportedLocomotion = RemoteLocomotion::Idle;
        } else if (motionState == kMotionStateWalk) {
            reportedLocomotion = RemoteLocomotion::Walk;
        } else if (motionState == kMotionStateRun) {
            reportedLocomotion = RemoteLocomotion::Run;
        } else if (motionState == kMotionStateSprint) {
            reportedLocomotion = RemoteLocomotion::Sprint;
        }
        const auto visualLocomotion =
            SelectDirectReplicaVisualLocomotion(
                reportedLocomotion,
                state.desiredMoveBlend);
        const auto visualDirection =
            ClassifyRemoteMovementDirection(
                state.localForwardSpeed,
                state.localRightSpeed);
        const auto taskAgeMs =
            animGraphVisualTaskActive_ &&
                    now >= animGraphVisualTaskStartedMs_
                ? now - animGraphVisualTaskStartedMs_
                : std::numeric_limits<std::uint64_t>::max();
        const bool aimTargetChanged =
            requestedAiming &&
            Distance(previousRemoteAimTarget_, state.aimTarget) >=
                kRemotePlayerAimTargetRefreshMeters;
        const bool aimRefreshExpired =
            requestedAiming &&
            (previousRemoteAimTaskMs_ == 0U ||
             now < previousRemoteAimTaskMs_ ||
             now - previousRemoteAimTaskMs_ >=
                 kRemotePlayerAimTaskRefreshMilliseconds);
        const bool refreshVisualTask =
            ShouldRefreshDirectReplicaVisualTask(
                DirectReplicaVisualTaskRefreshInput{
                    animGraphVisualTaskActive_,
                    animGraphVisualLocomotion_,
                    visualLocomotion,
                    taskAgeMs,
                    AbsoluteHeadingDifference(
                        animGraphVisualTaskHeading_,
                        visualLocomotion == RemoteLocomotion::Idle
                            ? state.heading
                            : state.movementHeading),
                    animGraphVisualDirection_,
                    visualDirection}) ||
            aimTargetChanged || aimRefreshExpired;
        if (refreshVisualTask) {
            const bool replacingTask = animGraphVisualTaskActive_;
            const bool directionChanged =
                animGraphVisualDirection_ != visualDirection;
            if (requestedAiming &&
                visualLocomotion == RemoteLocomotion::Idle) {
                AI::TASK_AIM_GUN_AT_COORD(
                    handle,
                    state.aimTarget.x,
                    state.aimTarget.y,
                    state.aimTarget.z,
                    kRemotePlayerAimTaskDurationMilliseconds,
                    FALSE,
                    FALSE);
                animGraphVisualTaskDestination_ = state.position;
                animGraphAimTaskDestination_ = state.position;
                ++animGraphAimIdleTaskStarts_;
            } else if (requestedAiming) {
                animGraphVisualTaskDestination_ =
                    ComputeDirectReplicaVisualTaskDestination(
                        state.position,
                        state.velocity,
                        state.movementHeading,
                        visualLocomotion);
                animGraphAimTaskDestination_ =
                    animGraphVisualTaskDestination_;
                // One native task owns both the lower-body path and the
                // upper-body weapon pose. Running a separate straight-to task
                // here would continually overwrite the aiming graph.
                AI::TASK_GO_TO_COORD_WHILE_AIMING_AT_COORD(
                    handle,
                    animGraphVisualTaskDestination_.x,
                    animGraphVisualTaskDestination_.y,
                    animGraphVisualTaskDestination_.z,
                    state.aimTarget.x,
                    state.aimTarget.y,
                    state.aimTarget.z,
                    DirectReplicaVisualTaskSpeed(visualLocomotion),
                    FALSE,
                    0.05F,
                    0.05F,
                    TRUE,
                    0,
                    FALSE,
                    0,
                    0);
                ++animGraphAimMovingTaskStarts_;
            } else if (visualLocomotion == RemoteLocomotion::Idle) {
                if (nativeIdleTurnRequested) {
                    // A heading warp reproduces position but skips the
                    // sender's feet/hip turn. Let the native graph perform a
                    // bounded turn while the network root remains fixed.
                    AI::TASK_ACHIEVE_HEADING(
                        handle,
                        state.heading,
                        kAnimGraphTurnInPlaceTimeoutMilliseconds);
                    ++animGraphTurnInPlaceTaskStarts_;
                } else {
                    AI::TASK_STAND_STILL(
                        handle,
                        kAnimGraphVisualIdleTaskMilliseconds);
                }
                animGraphVisualTaskDestination_ = state.position;
            } else {
                animGraphVisualTaskDestination_ =
                    ComputeDirectReplicaVisualTaskDestination(
                        state.position,
                        state.velocity,
                        state.movementHeading,
                        visualLocomotion);
                AI::TASK_GO_STRAIGHT_TO_COORD(
                    handle,
                    animGraphVisualTaskDestination_.x,
                    animGraphVisualTaskDestination_.y,
                    animGraphVisualTaskDestination_.z,
                    DirectReplicaVisualTaskSpeed(visualLocomotion),
                    kAnimGraphVisualTaskTimeoutMilliseconds,
                    state.movementHeading,
                    kAnimGraphVisualTaskStoppingRangeMeters,
                    0);
            }
            PED::SET_PED_KEEP_TASK(handle, TRUE);
            animGraphVisualLocomotion_ = visualLocomotion;
            animGraphVisualDirection_ = visualDirection;
            animGraphVisualTaskHeading_ =
                visualLocomotion == RemoteLocomotion::Idle
                    ? state.heading
                    : state.movementHeading;
            animGraphVisualTaskStartedMs_ = now;
            animGraphVisualTaskActive_ = true;
            if (requestedAiming) {
                previousRemoteAimTaskMs_ = now;
                previousRemoteAimTarget_ = state.aimTarget;
            }
            ++animGraphVisualTaskStarts_;
            if (replacingTask) {
                ++animGraphVisualTaskRefreshes_;
            }
            if (directionChanged) {
                ++animGraphDirectionTransitions_;
            }
        }
        AI::SET_PED_DESIRED_MOVE_BLEND_RATIO(
            handle,
            std::clamp(state.desiredMoveBlend, 0.0F, 3.0F));
        PED::SET_PED_MAX_MOVE_BLEND_RATIO(
            handle,
            std::clamp(
                std::max(state.desiredMoveBlend, 1.0F),
                1.0F,
                3.0F));
        // RDR2 restores move rate itself, so this neutral override is asserted
        // each game tick. Unlike the old motor it is never used for catch-up.
        PED::SET_PED_MOVE_RATE_OVERRIDE(handle, 1.0F);
        ENTITY::FORCE_ENTITY_AI_AND_ANIMATION_UPDATE(handle, FALSE);
        ++animGraphReplicaStateApplies_;

        const bool expectedMoving =
            visualLocomotion != RemoteLocomotion::Idle;
        if (requestedAiming) {
            ++animGraphAimExpectedTicks_;
        }
        const bool observedMoving =
            AI::IS_PED_WALKING(handle) != FALSE ||
            AI::IS_PED_RUNNING(handle) != FALSE ||
            AI::IS_PED_SPRINTING(handle) != FALSE ||
            AI::IS_MOVE_BLEND_RATIO_WALKING(handle) != FALSE ||
            AI::IS_MOVE_BLEND_RATIO_RUNNING(handle) != FALSE ||
            AI::IS_MOVE_BLEND_RATIO_SPRINTING(handle) != FALSE;
        if (expectedMoving) {
            ++animGraphExpectedMovingTicks_;
            if (observedMoving) {
                ++animGraphObservedMovingTicks_;
                animGraphMissingLocomotionSinceMs_ = 0U;
            } else {
                ++animGraphMissingLocomotionTicks_;
                if (animGraphMissingLocomotionSinceMs_ == 0U ||
                    now < animGraphMissingLocomotionSinceMs_) {
                    animGraphMissingLocomotionSinceMs_ = now;
                }
                const bool recoveryDue =
                    now - animGraphMissingLocomotionSinceMs_ >=
                        kAnimGraphMissingLocomotionRecoveryMilliseconds &&
                    (animGraphPreviousLocomotionRecoveryMs_ == 0U ||
                     now < animGraphPreviousLocomotionRecoveryMs_ ||
                     now - animGraphPreviousLocomotionRecoveryMs_ >=
                         kAnimGraphLocomotionRecoveryCooldownMilliseconds);
                if (recoveryDue) {
                    // A valid long-lived straight-to task can become inert
                    // after camp speed zones or a Story script briefly owns
                    // the proxy. Merely refreshing its destination preserves
                    // that dead task. Reacquire the visual graph once after a
                    // measured 350ms stall; direct-root position authority is
                    // unaffected and the two-second cooldown prevents flicker.
                    AI::CLEAR_PED_TASKS(handle, TRUE, TRUE);
                    PED::SET_PED_KEEP_TASK(handle, FALSE);
                    animGraphVisualTaskActive_ = false;
                    animGraphVisualTaskStartedMs_ = 0U;
                    animGraphPreviousLocomotionRecoveryMs_ = now;
                    animGraphMissingLocomotionSinceMs_ = 0U;
                    ++animGraphLocomotionRecoveries_;
                    Log(
                        "[WARNING][ANIMGRAPH_REPLICA] visual locomotion stalled for 350ms; native gait task reacquire scheduled");
                }
            }
        } else {
            animGraphMissingLocomotionSinceMs_ = 0U;
        }
    }

    constexpr auto kFiring = static_cast<std::uint32_t>(
        PlayerStateFlag::Firing);
    if (state.fireSequence != 0U) {
        const auto disposition =
            remoteFireSequences_.Observe(state.fireSequence);
        const bool firstCarriesShot =
            disposition == SequenceDisposition::First &&
            (state.flags & kFiring) != 0U;
        if ((firstCarriesShot ||
             disposition == SequenceDisposition::Newer) &&
            hasAimTarget && remoteWeaponHash_ != 0U &&
            !physicalAnimation) {
            ++remoteActionFireEvents_;
            const bool fireGraphPulseAllowed =
                !requestedCover && !mounted &&
                state.desiredMoveBlend < 0.20F;
            const bool fireGraphPulseStarted =
                fireGraphPulseAllowed &&
                StartRemoteVisualFirePulse(
                    handle,
                    state.aimTarget,
                    now);
            if (fireGraphPulseStarted) {
                animGraphVisualTaskActive_ = false;
                animGraphVisualTaskStartedMs_ = 0U;
                previousRemoteAimTaskMs_ = 0U;
            } else {
                if (!fireGraphPulseAllowed) {
                    ++remoteActionFireGraphPulseSuppressed_;
                }
                if (!requestedAiming && !requestedCover &&
                    state.desiredMoveBlend < 0.20F) {
                    // Keep the older pose-only fallback if the weapon has not
                    // streamed/equipped yet when this shot edge arrives.
                    AI::TASK_AIM_GUN_AT_COORD(
                        handle,
                        state.aimTarget.x,
                        state.aimTarget.y,
                        state.aimTarget.z,
                        180,
                        FALSE,
                        FALSE);
                    PED::SET_PED_KEEP_TASK(handle, TRUE);
                    animGraphVisualTaskActive_ = false;
                    animGraphVisualTaskStartedMs_ = 0U;
                    ++animGraphAimIdleTaskStarts_;
                }
            }
            GAMEPLAY::SHOOT_SINGLE_BULLET_BETWEEN_COORDS(
                state.position.x,
                state.position.y,
                state.position.z + 1.0F,
                state.aimTarget.x,
                state.aimTarget.y,
                state.aimTarget.z,
                0,
                TRUE,
                static_cast<Hash>(remoteWeaponHash_),
                handle,
                TRUE,
                FALSE,
                500.0F,
                FALSE);
            ++remoteActionVisualShots_;
            if (EnsureRemoteGunshotAudio()) {
                AUDIO::_0x6FB1DA3CA9DA7D90(
                    reinterpret_cast<Any*>(kRemoteGunshotSoundName),
                    static_cast<Any>(handle),
                    reinterpret_cast<Any*>(kRemoteGunshotSoundSet),
                    FALSE,
                    0,
                    0);
                ++remoteActionAudioShots_;
            } else {
                ++remoteActionAudioNotReady_;
            }
        }
    }

    constexpr auto kSyntheticTest = static_cast<std::uint32_t>(
        PlayerStateFlag::SyntheticTest);
    if ((state.flags & kSyntheticTest) != 0U) {
        float screenX{};
        float screenY{};
        if (GRAPHICS::GET_SCREEN_COORD_FROM_WORLD_COORD(
                state.position.x,
                state.position.y,
                state.position.z + 0.35F,
                &screenX,
                &screenY) != FALSE) {
            DrawNativeRectangle(
                screenX,
                screenY,
                0.026F,
                0.0028F,
                198,
                38,
                32,
                235);
            DrawNativeRectangle(
                screenX,
                screenY,
                0.0028F,
                0.046F,
                198,
                38,
                32,
                235);
            char markerText[80]{};
            std::snprintf(
                markerText,
                sizeof(markerText),
                "ANIMGRAPH TARGET  %.2f m",
                static_cast<double>(positionError));
            DrawNativeText(
                markerText,
                screenX,
                screenY - 0.032F,
                0.23F,
                244,
                230,
                196,
                245,
                true);
        }
    }

    ++animGraphReplicaTicks_;
    animGraphReplicaPositionErrorSum_ += positionError;
    animGraphReplicaPositionErrorMax_ = std::max(
        animGraphReplicaPositionErrorMax_,
        positionError);
    if (animGraphReplicaDiagnosticsStartedMs_ == 0U) {
        animGraphReplicaDiagnosticsStartedMs_ = now;
    }
    if (now >= animGraphReplicaDiagnosticsStartedMs_ &&
        now - animGraphReplicaDiagnosticsStartedMs_ >=
            kRemoteMotionDiagnosticsMilliseconds) {
        const auto meanError =
            animGraphReplicaTicks_ == 0U
                ? 0.0
                : animGraphReplicaPositionErrorSum_ /
                      static_cast<double>(animGraphReplicaTicks_);
        const auto source =
            latestRemoteAnimationState_.has_value()
                ? static_cast<std::uint8_t>(
                      latestRemoteAnimationState_->source)
                : 0U;
        Log(
            "[INFO][ANIMGRAPH_REPLICA] v30.3/cutscene-mission-lasso-recovery/5s: ticks=" +
            std::to_string(animGraphReplicaTicks_) +
            ", mean-pre-correction-error-m=" +
            std::to_string(meanError) +
            ", max-pre-correction-error-m=" +
            std::to_string(animGraphReplicaPositionErrorMax_) +
            ", coordinate-corrections=" +
            std::to_string(animGraphReplicaCorrections_) +
            ", driver-state-ticks=" +
            std::to_string(animGraphReplicaStateApplies_) +
            ", visual-task-active=" +
            std::to_string(animGraphVisualTaskActive_ ? 1 : 0) +
            ", visual-task-starts=" +
            std::to_string(animGraphVisualTaskStarts_) +
            ", visual-task-refreshes=" +
            std::to_string(animGraphVisualTaskRefreshes_) +
            ", direction-transitions=" +
            std::to_string(animGraphDirectionTransitions_) +
            ", turn-in-place-task-starts=" +
            std::to_string(animGraphTurnInPlaceTaskStarts_) +
            ", ik-preparations=" +
            std::to_string(animGraphIkPreparations_) +
            ", expected-moving-ticks=" +
            std::to_string(animGraphExpectedMovingTicks_) +
            ", observed-moving-ticks=" +
            std::to_string(animGraphObservedMovingTicks_) +
            ", missing-locomotion-ticks=" +
            std::to_string(animGraphMissingLocomotionTicks_) +
            ", locomotion-recoveries=" +
            std::to_string(animGraphLocomotionRecoveries_) +
            ", traversal-expected-ticks=" +
            std::to_string(animGraphTraversalExpectedTicks_) +
            ", traversal-observed-ticks=" +
            std::to_string(animGraphTraversalObservedTicks_) +
            ", traversal-missing-ticks=" +
            std::to_string(animGraphTraversalMissingTicks_) +
            ", jump-task-starts=" +
            std::to_string(animGraphTraversalJumpTaskStarts_) +
            ", climb-task-starts=" +
            std::to_string(animGraphTraversalClimbTaskStarts_) +
            ", airborne-launches=" +
            std::to_string(animGraphAirborneLaunches_) +
            ", physical-root-yield-ticks=" +
            std::to_string(animGraphPhysicalRootYieldTicks_) +
            ", physical-root-leash-corrections=" +
            std::to_string(animGraphPhysicalRootLeashCorrections_) +
            ", ragdoll-task-starts=" +
            std::to_string(animGraphRagdollTaskStarts_) +
            ", stealth-expected-ticks=" +
            std::to_string(animGraphStealthExpectedTicks_) +
            ", stealth-observed-ticks=" +
            std::to_string(animGraphStealthObservedTicks_) +
            ", stealth-transitions=" +
            std::to_string(animGraphStealthTransitions_) +
            ", stealth-recoveries=" +
            std::to_string(animGraphStealthRecoveries_) +
            ", water-expected-ticks=" +
            std::to_string(animGraphWaterExpectedTicks_) +
            ", water-observed-ticks=" +
            std::to_string(animGraphWaterObservedTicks_) +
            ", swimming-expected-ticks=" +
            std::to_string(animGraphSwimmingExpectedTicks_) +
            ", swimming-observed-ticks=" +
            std::to_string(animGraphSwimmingObservedTicks_) +
            ", cover-expected-ticks=" +
            std::to_string(animGraphCoverExpectedTicks_) +
            ", cover-observed-ticks=" +
            std::to_string(animGraphCoverObservedTicks_) +
            ", cover-transitions=" +
            std::to_string(animGraphCoverTransitions_) +
            ", cover-task-starts=" +
            std::to_string(animGraphCoverTaskStarts_) +
            ", cover-task-cancels=" +
            std::to_string(animGraphCoverTaskCancels_) +
            ", cover-fallback-starts=" +
            std::to_string(animGraphCoverFallbackStarts_) +
            ", cover-reacquires=" +
            std::to_string(animGraphCoverReacquires_) +
            ", cover-fallback-recoveries=" +
            std::to_string(animGraphCoverFallbackRecoveries_) +
            ", fire-graph-pulses=" +
            std::to_string(remoteActionFireGraphPulses_) +
            ", fire-events=" +
            std::to_string(remoteActionFireEvents_) +
            ", fire-graph-suppressed=" +
            std::to_string(remoteActionFireGraphPulseSuppressed_) +
            ", fire-ammo-restores=" +
            std::to_string(remoteActionFireAmmoRestores_) +
            ", visual-zero-damage-shots=" +
            std::to_string(remoteActionVisualShots_) +
            ", spatial-audio-shots=" +
            std::to_string(remoteActionAudioShots_) +
            ", audio-not-ready=" +
            std::to_string(remoteActionAudioNotReady_) +
            ", melee-expected-ticks=" +
            std::to_string(animGraphMeleeExpectedTicks_) +
            ", melee-task-starts=" +
            std::to_string(animGraphMeleeTaskStarts_) +
            ", melee-missing-target-ticks=" +
            std::to_string(animGraphMeleeMissingTargetTicks_) +
            ", melee-task-cancels=" +
            std::to_string(animGraphMeleeTaskCancels_) +
            ", aim-expected-ticks=" +
            std::to_string(animGraphAimExpectedTicks_) +
            ", aim-idle-task-starts=" +
            std::to_string(animGraphAimIdleTaskStarts_) +
            ", aim-moving-task-starts=" +
            std::to_string(animGraphAimMovingTaskStarts_) +
            ", aim-task-cancels=" +
            std::to_string(animGraphAimTaskCancels_) +
            ", traversal-expired=" +
            std::to_string(animGraphTraversalExpired_) +
            ", traversal-pending=" +
            std::to_string(pendingRemoteTraversals_.size()) +
            ", sample-source=" + std::to_string(source) +
            ", move-network-samples=" +
            std::to_string(animGraphReplicaMoveNetworkSamples_) +
            ", unavailable-samples=" +
            std::to_string(animGraphReplicaUnavailableSamples_) +
            ", visual-task-owner=1, task-navmesh-owner=0");
        animGraphReplicaDiagnosticsStartedMs_ = now;
        animGraphReplicaTicks_ = 0U;
        animGraphReplicaCorrections_ = 0U;
        animGraphReplicaStateApplies_ = 0U;
        animGraphVisualTaskStarts_ = 0U;
        animGraphVisualTaskRefreshes_ = 0U;
        animGraphDirectionTransitions_ = 0U;
        animGraphTurnInPlaceTaskStarts_ = 0U;
        animGraphIkPreparations_ = 0U;
        animGraphExpectedMovingTicks_ = 0U;
        animGraphObservedMovingTicks_ = 0U;
        animGraphMissingLocomotionTicks_ = 0U;
        animGraphLocomotionRecoveries_ = 0U;
        animGraphTraversalExpectedTicks_ = 0U;
        animGraphTraversalObservedTicks_ = 0U;
        animGraphTraversalMissingTicks_ = 0U;
        animGraphTraversalJumpTaskStarts_ = 0U;
        animGraphTraversalClimbTaskStarts_ = 0U;
        animGraphAirborneLaunches_ = 0U;
        animGraphPhysicalRootYieldTicks_ = 0U;
        animGraphPhysicalRootLeashCorrections_ = 0U;
        animGraphRagdollTaskStarts_ = 0U;
        animGraphStealthExpectedTicks_ = 0U;
        animGraphStealthObservedTicks_ = 0U;
        animGraphStealthTransitions_ = 0U;
        animGraphStealthRecoveries_ = 0U;
        animGraphWaterExpectedTicks_ = 0U;
        animGraphWaterObservedTicks_ = 0U;
        animGraphSwimmingExpectedTicks_ = 0U;
        animGraphSwimmingObservedTicks_ = 0U;
        animGraphCoverExpectedTicks_ = 0U;
        animGraphCoverObservedTicks_ = 0U;
        animGraphCoverTransitions_ = 0U;
        animGraphCoverTaskStarts_ = 0U;
        animGraphCoverTaskCancels_ = 0U;
        animGraphCoverFallbackStarts_ = 0U;
        animGraphCoverReacquires_ = 0U;
        animGraphCoverFallbackRecoveries_ = 0U;
        remoteActionFireGraphPulses_ = 0U;
        remoteActionFireGraphPulseSuppressed_ = 0U;
        remoteActionFireAmmoRestores_ = 0U;
        remoteActionFireEvents_ = 0U;
        remoteActionVisualShots_ = 0U;
        remoteActionAudioShots_ = 0U;
        remoteActionAudioNotReady_ = 0U;
        animGraphMeleeExpectedTicks_ = 0U;
        animGraphMeleeTaskStarts_ = 0U;
        animGraphMeleeMissingTargetTicks_ = 0U;
        animGraphMeleeTaskCancels_ = 0U;
        animGraphAimExpectedTicks_ = 0U;
        animGraphAimIdleTaskStarts_ = 0U;
        animGraphAimMovingTaskStarts_ = 0U;
        animGraphAimTaskCancels_ = 0U;
        animGraphTraversalExpired_ = 0U;
        animGraphReplicaMoveNetworkSamples_ = 0U;
        animGraphReplicaUnavailableSamples_ = 0U;
        animGraphReplicaPositionErrorSum_ = 0.0;
        animGraphReplicaPositionErrorMax_ = 0.0F;
    }
    return true;
#else
    (void)handle;
    (void)state;
    return false;
#endif
}

bool ScriptHookSdkFacade::ApplyRemoteTransform(
    const PlayerStatePayload& state) noexcept {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS && defined(_MSC_VER)
    // One stale/recycled RDR2 ped handle produced V31.2's only recorded
    // 0xC0000005 and caused the top-level bridge guard to abandon the complete
    // coop runtime. Keep this high-frequency native presentation lane behind
    // its own SEH boundary. Returning false lets BridgeRuntime discard and
    // recreate the replica after its bounded three-failure policy.
    bool nativeFault{};
    bool applied{};
    __try {
        applied = ApplyRemoteTransformUnsafe(state);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        nativeFault = true;
    }
    if (nativeFault) {
        if (!remoteTransformNativeFaultLogged_) {
            remoteTransformNativeFaultLogged_ = true;
            Log(
                "[ERROR][REMOTE_TRANSFORM][SEH_RECOVERED] native presentation fault isolated; stale replica will be respawned instead of stopping coop");
        }
        return false;
    }
    remoteTransformNativeFaultLogged_ = false;
    return applied;
#else
    return ApplyRemoteTransformUnsafe(state);
#endif
}

bool ScriptHookSdkFacade::ApplyRemoteTransformUnsafe(
    const PlayerStatePayload& state) noexcept {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
    constexpr auto kAiming =
        static_cast<std::uint32_t>(
            PlayerStateFlag::Aiming);
    constexpr auto kFiring =
        static_cast<std::uint32_t>(
            PlayerStateFlag::Firing);
    constexpr auto kAimTargetValid =
        static_cast<std::uint32_t>(
            PlayerStateFlag::AimTargetValid);
    constexpr auto kMeleeCombat =
        static_cast<std::uint32_t>(
            PlayerStateFlag::MeleeCombat);
    constexpr auto kPeerCombatTarget =
        static_cast<std::uint32_t>(
            PlayerStateFlag::PeerCombatTarget);
    constexpr auto kPeerLassoActive =
        static_cast<std::uint32_t>(
            PlayerStateFlag::PeerLassoActive);
    constexpr auto kMeleeBlocking =
        static_cast<std::uint32_t>(
            PlayerStateFlag::MeleeBlocking);
    constexpr auto kMeleeGrappling =
        static_cast<std::uint32_t>(
            PlayerStateFlag::MeleeGrappling);
    constexpr auto kPeerKnockdown =
        static_cast<std::uint32_t>(
            PlayerStateFlag::PeerKnockdown);
    constexpr auto kMounted =
        static_cast<std::uint32_t>(
            PlayerStateFlag::Mounted);
    constexpr auto kJumping =
        static_cast<std::uint32_t>(
            PlayerStateFlag::Jumping);
    constexpr auto kClimbing =
        static_cast<std::uint32_t>(
            PlayerStateFlag::Climbing);
    constexpr auto kSyntheticTest =
        static_cast<std::uint32_t>(
            PlayerStateFlag::SyntheticTest);
    const bool syntheticTest =
        (state.flags & kSyntheticTest) != 0U;
    const bool replicatedMounted =
        (state.flags & kMounted) != 0U &&
        remotePlayerMounted_ &&
        remotePlayerMountHandle_ != 0 &&
        ENTITY::DOES_ENTITY_EXIST(
            static_cast<Entity>(
                remotePlayerMountHandle_)) != FALSE;
    const bool hasAimTarget =
        (state.flags & kAimTargetValid) != 0U;
    if (!state.entityId.IsValid() ||
        !IsFinite(state.position) ||
        !IsFinite(state.velocity) ||
        !std::isfinite(state.heading) ||
        !IsFinite(state.aimTarget) ||
        !std::isfinite(state.movementHeading) ||
        !std::isfinite(state.localForwardSpeed) ||
        !std::isfinite(state.localRightSpeed) ||
        !std::isfinite(state.desiredMoveBlend) ||
        state.desiredMoveBlend < 0.0F ||
        state.desiredMoveBlend > 3.0F ||
        !IsFinite(state.traversalAnchor) ||
        !std::isfinite(state.traversalHeading) ||
        (!hasAimTarget &&
         (state.aimTarget.x != 0.0F ||
          state.aimTarget.y != 0.0F ||
          state.aimTarget.z != 0.0F))) {
        return false;
    }

    const auto now = TickMilliseconds();
    ExpireRemotePlayerActions(now);
    MaintainPendingPeerDismount(now);
    if (remoteMeleeVisualActionId_ != 0U &&
        remoteMeleeVisualDeadlineMs_ != 0U &&
        now >= remoteMeleeVisualDeadlineMs_) {
        const auto expiredActionId = remoteMeleeVisualActionId_;
        ++remotePlayerActionMeleeDeadlineCancels_;
        CancelRemoteMeleeVisual(0, "bounded-deadline");
        auto& meleeChannel =
            remotePlayerActionChannels_[static_cast<std::size_t>(
                PlayerActionKind::MeleeAttack)];
        if (meleeChannel.active &&
            meleeChannel.actionId == expiredActionId) {
            meleeChannel.active = false;
            meleeChannel.phase = PlayerActionPhase::Cancel;
            meleeChannel.targetsLocalPlayer = false;
            meleeChannel.physicalTargetEffect = false;
            RefreshRemotePlayerActionDerivedState();
            Log(
                "[WARNING][ACTION_FSM] bounded melee deadline closed the semantic channel before network End: action-id=" +
                std::to_string(expiredActionId));
        }
    }
    if (remotePlayerActionDiagnosticsStartedMs_ == 0U) {
        remotePlayerActionDiagnosticsStartedMs_ = now;
    }
    if (now >= remotePlayerActionDiagnosticsStartedMs_ &&
        now - remotePlayerActionDiagnosticsStartedMs_ >=
            kRemoteMotionDiagnosticsMilliseconds) {
        Log(
            "[INFO][ACTION_APPLY] v22/deterministic-fsm/5s: protocol-observed=" +
            std::to_string(reliablePlayerActionProtocolObserved_) +
            ", active-aim=" + std::to_string(remoteActionAimActive_) +
            ", active-melee=" + std::to_string(remoteActionMeleeActive_) +
            ", active-block=" + std::to_string(remoteActionBlockActive_) +
            ", active-grapple=" + std::to_string(remoteActionGrappleActive_) +
            ", active-lasso=" + std::to_string(remoteActionLassoActive_) +
            ", active-knockdown=" + std::to_string(remoteActionKnockdownActive_) +
            ", begins=" + std::to_string(remotePlayerActionBegins_) +
            ", sustains=" + std::to_string(remotePlayerActionSustains_) +
            ", terminals=" + std::to_string(remotePlayerActionTerminals_) +
            ", preemptions=" + std::to_string(remotePlayerActionPreemptions_) +
            ", stale-revisions=" + std::to_string(remotePlayerActionStaleRevisions_) +
            ", epoch-rejects=" + std::to_string(remotePlayerActionEpochRejects_) +
            ", foreign-terminals=" + std::to_string(remotePlayerActionForeignTerminals_) +
            ", watchdog-timeouts=" + std::to_string(remotePlayerActionTimeouts_) +
            ", native-cancels=" + std::to_string(remotePlayerActionNativeCancels_) +
            ", melee-one-shots=" + std::to_string(remotePlayerActionMeleeVisualStarts_) +
            ", melee-deadline-cancels=" + std::to_string(remotePlayerActionMeleeDeadlineCancels_) +
            ", melee-semantic-only=" + std::to_string(remotePlayerActionMeleeSemanticOnly_) +
            ", lasso-pending=" + std::to_string(remotePlayerActionLassoPending_) +
            ", lasso-confirmed=" + std::to_string(remotePlayerActionLassoConfirmed_) +
            ", lasso-failed=" + std::to_string(remotePlayerActionLassoFailed_) +
            ", rope-releases=" + std::to_string(remotePlayerActionRopeDeletes_) +
            ", victim-fallbacks=" + std::to_string(remotePlayerActionVictimFallbacks_) +
            ", motor-yields=" + std::to_string(remotePlayerActionMotorYields_) +
            ", dismount-requests=" + std::to_string(remotePlayerActionDismountRequests_) +
            ", dismount-retries=" + std::to_string(remotePlayerActionDismountRetries_) +
            ", dismount-confirmed=" + std::to_string(remotePlayerActionDismountConfirmed_) +
            ", dismount-failed=" + std::to_string(remotePlayerActionDismountFailed_) +
            ", melee-owner-action-id=" + std::to_string(remoteMeleeVisualActionId_) +
            ", dismount-pending-action-id=" + std::to_string(remotePeerDismountActionId_) +
            ", constraint-pending=" + std::to_string(remotePeerLassoTaskPending_) +
            ", constraint-engine-owned=" + std::to_string(remotePeerLassoEngineOwned_));
        remotePlayerActionDiagnosticsStartedMs_ = now;
        remotePlayerActionBegins_ = 0U;
        remotePlayerActionSustains_ = 0U;
        remotePlayerActionTerminals_ = 0U;
        remotePlayerActionPreemptions_ = 0U;
        remotePlayerActionStaleRevisions_ = 0U;
        remotePlayerActionEpochRejects_ = 0U;
        remotePlayerActionForeignTerminals_ = 0U;
        remotePlayerActionNativeCancels_ = 0U;
        remotePlayerActionMeleeVisualStarts_ = 0U;
        remotePlayerActionMeleeDeadlineCancels_ = 0U;
        remotePlayerActionMeleeSemanticOnly_ = 0U;
        remotePlayerActionRopeCreates_ = 0U;
        remotePlayerActionRopeDeletes_ = 0U;
        remotePlayerActionTimeouts_ = 0U;
        remotePlayerActionLassoPending_ = 0U;
        remotePlayerActionLassoConfirmed_ = 0U;
        remotePlayerActionLassoFailed_ = 0U;
        remotePlayerActionVictimFallbacks_ = 0U;
        remotePlayerActionMotorYields_ = 0U;
        remotePlayerActionDismountRequests_ = 0U;
        remotePlayerActionDismountRetries_ = 0U;
        remotePlayerActionDismountConfirmed_ = 0U;
        remotePlayerActionDismountFailed_ = 0U;
    }
    const bool peerCombatTarget =
        (state.flags & kPeerCombatTarget) != 0U;
    const bool requestedPeerLasso =
        reliablePlayerActionProtocolObserved_
            ? remoteActionLassoActive_
            : peerCombatTarget &&
                  (state.flags & kPeerLassoActive) != 0U;
    const bool requestedPeerKnockdown =
        reliablePlayerActionProtocolObserved_
            ? remoteActionKnockdownActive_
            : peerCombatTarget &&
                  (state.flags & kPeerKnockdown) != 0U;
    if ((peerCombatTarget &&
         (((state.flags & kMeleeCombat) != 0U) ||
          ((state.flags & kMeleeBlocking) != 0U) ||
          ((state.flags & kMeleeGrappling) != 0U))) ||
        remoteActionMeleeActive_ || remoteActionBlockActive_ ||
        remoteActionGrappleActive_ || requestedPeerLasso ||
        requestedPeerKnockdown) {
        peerCombatIsolationUntilMs_ =
            now + kPeerCombatIsolationHoldMilliseconds;
    }
    if (requestedPeerLasso != remotePeerLassoActive_) {
        remotePeerLassoActive_ = requestedPeerLasso;
        previousPeerLassoRagdollMs_ = 0U;
        Log(
            requestedPeerLasso
                ? "[INFO][PEER_COMBAT] remote lasso intent started; native rope task owns victim physics"
                : "[INFO][PEER_COMBAT] remote lasso restraint released");
    }
    if (requestedPeerKnockdown != remotePeerKnockdownActive_) {
        remotePeerKnockdownActive_ = requestedPeerKnockdown;
        if (!requestedPeerKnockdown ||
            remotePeerDismountActionId_ == 0U) {
            previousPeerLassoRagdollMs_ = 0U;
        }
        Log(
            requestedPeerKnockdown
                ? "[INFO][PEER_COMBAT] remote melee knockdown accepted locally"
                : "[INFO][PEER_COMBAT] remote melee knockdown released");
    }

    const auto handle = replicas_.FindLocal(state.entityId);
    if (!handle.has_value() ||
        ENTITY::DOES_ENTITY_EXIST(*handle) == FALSE) {
        if (handle.has_value()) {
            (void)replicas_.Remove(state.entityId);
        }
        if (state.entityId == remotePlayerId_) {
            ReleaseRemoteGunshotAudio();
            ClearRemoteIdentityDecoration();
            remoteWeaponHash_ = 0U;
            remoteReloading_ = false;
            remoteAppearanceFingerprint_ = 0U;
            remoteAppearanceModelHash_ = 0U;
            remoteAppearanceComponents_.clear();
            remotePlayerId_ = NetEntityId{};
            ResetRemoteMotionTracking();
        }
        return false;
    }
    if (IsOwnedHybridAnimSceneEntity(*handle)) {
        // An exact AnimScene role must have a single animation/root-motion
        // owner. The replicated transform remains cached by BridgeRuntime and
        // resumes after the scene is aborted.
        return true;
    }
    if (requestedPeerLasso || requestedPeerKnockdown) {
        const auto localPed = PLAYER::PLAYER_PED_ID();
        if (localPed != 0 &&
            ENTITY::DOES_ENTITY_EXIST(localPed) != FALSE) {
            const bool nativeLassoConstraint =
                requestedPeerLasso &&
                (IsPedLassoed(localPed) != FALSE ||
                 IsPedBeingHogtied(localPed) != FALSE ||
                 IsPedHogtied(localPed) != FALSE ||
                 GetLassoTarget(static_cast<Ped>(*handle)) == localPed);
            if (nativeLassoConstraint &&
                !remotePeerLassoEngineOwned_) {
                remotePeerLassoTaskPending_ = false;
                remotePeerLassoEngineOwned_ = true;
                ++remotePlayerActionRopeCreates_;
                ++remotePlayerActionLassoConfirmed_;
                Log("[LASSO_ROPE] engine ownership observed from transform tick");
            }
            // A missed or delayed rope is allowed to miss. Replacing it with
            // SET_PED_TO_RAGDOLL made both peers fall without a line and could
            // interrupt TASK_LASSO_PED before its controlled retry. Only an
            // explicit authoritative Knockdown may use the ragdoll path.
            const bool shouldApplyRagdoll = requestedPeerKnockdown;
            const bool fallbackRefreshAllowed =
                reliablePlayerActionProtocolObserved_
                    ? previousPeerLassoRagdollMs_ == 0U
                    : previousPeerLassoRagdollMs_ == 0U ||
                          now < previousPeerLassoRagdollMs_ ||
                          now - previousPeerLassoRagdollMs_ >=
                              kPeerKnockdownRagdollRefreshMilliseconds;
            const auto localPosition = ToBridgeVector(
                ENTITY::GET_ENTITY_COORDS(
                    localPed,
                    TRUE,
                    FALSE));
            const auto remotePosition = ToBridgeVector(
                ENTITY::GET_ENTITY_COORDS(
                    *handle,
                    TRUE,
                    FALSE));
            if (shouldApplyRagdoll && fallbackRefreshAllowed &&
                Distance(localPosition, remotePosition) <=
                    kPeerLassoMaximumDistanceMeters) {
                (void)PED::SET_PED_TO_RAGDOLL(
                    localPed,
                    kPeerKnockdownRagdollDurationMilliseconds,
                    kPeerKnockdownRagdollDurationMilliseconds,
                    0,
                    TRUE,
                    TRUE,
                    FALSE);
                previousPeerLassoRagdollMs_ = now;
                ++remotePlayerActionVictimFallbacks_;
                Log(
                    "[VICTIM_CONSTRAINT] authoritative zero-damage knockdown applied once");
            }
        }
    }
    // Story scripts can restore cloned Arthur's friendly target flags after a
    // checkpoint or interaction. Keep the visual proxy targetable for
    // zero-damage weapon/lasso acquisition without ever granting local health
    // authority.
    ENTITY::SET_ENTITY_CAN_BE_TARGETED_WITHOUT_LOS(
        *handle,
        TRUE);
    PED::SET_PED_CAN_BE_TARGETTED(*handle, TRUE);
    PED::SET_PED_CAN_BE_TARGETTED_BY_PLAYER(
        *handle,
        PLAYER::PLAYER_ID(),
        TRUE);
    // Reassert visibility because Story scripts and population transitions can
    // hide a mission-owned MetaPed after it has already spawned.
    ENTITY::SET_ENTITY_VISIBLE(*handle, TRUE);
    ENTITY::RESET_ENTITY_ALPHA(*handle);
    if (remoteWeaponHash_ != 0U &&
        (previousRemoteWeaponVisualMs_ == 0U ||
         now < previousRemoteWeaponVisualMs_ ||
         now - previousRemoteWeaponVisualMs_ >=
             kRemoteWeaponVisualRefreshMilliseconds) &&
        PED::IS_PED_RAGDOLL(*handle) == FALSE &&
        PED::IS_PED_FALLING(*handle) == FALSE &&
        AI::IS_PED_GETTING_UP(*handle) == FALSE) {
        const auto requestedWeapon =
            static_cast<Hash>(remoteWeaponHash_);
        Hash currentWeapon{};
        const bool currentKnown =
            WEAPON::GET_CURRENT_PED_WEAPON(
                *handle,
                &currentWeapon,
                FALSE,
                0,
                FALSE) != FALSE;
        if ((!currentKnown || currentWeapon != requestedWeapon) &&
            (remoteWeaponHash_ == kWeaponUnarmed ||
             WEAPON::HAS_PED_GOT_WEAPON(
                 *handle,
                 requestedWeapon,
                 FALSE,
                 FALSE) != FALSE)) {
            WEAPON::SET_CURRENT_PED_WEAPON(
                *handle,
                requestedWeapon,
                TRUE,
                0,
                FALSE,
                FALSE);
        }
        previousRemoteWeaponVisualMs_ = now;
    }
    if (remoteWeaponHash_ != 0U &&
        !remoteGunshotSoundSetReady_) {
        (void)EnsureRemoteGunshotAudio();
    }

    if (!remoteNickname_.empty()) {
        const auto localPed = PLAYER::PLAYER_PED_ID();
        const auto remotePosition = ENTITY::GET_ENTITY_COORDS(
            *handle,
            TRUE,
            FALSE);
        if (localPed != 0 &&
            ENTITY::DOES_ENTITY_EXIST(localPed) != FALSE &&
            ENTITY::IS_ENTITY_VISIBLE_TO_SCRIPT(*handle) != FALSE &&
            ENTITY::HAS_ENTITY_CLEAR_LOS_TO_ENTITY(
                localPed,
                *handle,
                17) != FALSE) {
            const auto localPosition = ENTITY::GET_ENTITY_COORDS(
                localPed,
                TRUE,
                FALSE);
            const auto distance = Distance(
                ToBridgeVector(localPosition),
                ToBridgeVector(remotePosition));
            if (std::isfinite(distance) && distance <= 75.0F) {
                float screenX{};
                float screenY{};
                const auto visualPosition =
                    ToBridgeVector(remotePosition);
                // Keep the label attached to the entity the player can
                // actually see and target. Filtering removes tiny vertical
                // jitter without allowing the network root to separate the
                // nickname from a locally drifting puppet.
                const Vec3 nicknameTarget{
                    visualPosition.x,
                    visualPosition.y,
                    visualPosition.z + 1.15F};
                if (!hasRemoteNicknameAnchor_ ||
                    Distance(
                        remoteNicknameAnchor_,
                        nicknameTarget) >= 5.0F) {
                    remoteNicknameAnchor_ =
                        nicknameTarget;
                    hasRemoteNicknameAnchor_ = true;
                } else {
                    constexpr float kNicknameHorizontalAlpha =
                        0.55F;
                    constexpr float kNicknameVerticalAlpha =
                        0.10F;
                    remoteNicknameAnchor_ = {
                        remoteNicknameAnchor_.x +
                            (nicknameTarget.x -
                             remoteNicknameAnchor_.x) *
                                kNicknameHorizontalAlpha,
                        remoteNicknameAnchor_.y +
                            (nicknameTarget.y -
                             remoteNicknameAnchor_.y) *
                                kNicknameHorizontalAlpha,
                        remoteNicknameAnchor_.z +
                            (nicknameTarget.z -
                             remoteNicknameAnchor_.z) *
                                kNicknameVerticalAlpha};
                }
                if (GRAPHICS::GET_SCREEN_COORD_FROM_WORLD_COORD(
                        remoteNicknameAnchor_.x,
                        remoteNicknameAnchor_.y,
                        remoteNicknameAnchor_.z,
                        &screenX,
                        &screenY) != FALSE) {
                    const auto scale = std::clamp(
                        0.30F - (distance * 0.0015F),
                        0.20F,
                        0.30F);
                    // DRAW_TEXT uses RDR2's built-in UI typeface. ScriptHook
                    // exposes no per-call font selector, so the nameplate
                    // stays fully native without shipping a third-party TTF.
                    const auto nameWidth = std::clamp(
                        0.020F +
                            static_cast<float>(
                                remoteNickname_.size()) *
                                0.0035F,
                        0.050F,
                        0.160F);
                    DrawNativeRectangle(
                        screenX,
                        screenY + 0.012F,
                        nameWidth,
                        0.028F,
                        12,
                        10,
                        8,
                        135);
                    DrawNativeRectangle(
                        screenX,
                        screenY + 0.027F,
                        nameWidth * 0.92F,
                        0.0015F,
                        126,
                        24,
                        20,
                        220);
                    DrawNativeText(
                        remoteNickname_,
                        screenX,
                        screenY,
                        scale,
                        244,
                        230,
                        196,
                        245,
                        true);
                }
            }
        }
    }

    if (authoritativeRemoteRestraint_ == PlayerRestraintState::Lassoed &&
        authoritativeRemoteRestraintSubject_ == state.entityId &&
        authoritativeRemoteRestraintReceivedMs_ != 0U &&
        (now < authoritativeRemoteRestraintReceivedMs_ ||
         now - authoritativeRemoteRestraintReceivedMs_ >
             kAuthoritativeLassoLeaseMilliseconds)) {
        DeleteRemotePeerLassoRope();
        authoritativeRemoteRestraint_ = PlayerRestraintState::Free;
        authoritativeRemoteRestraintSubject_ = NetEntityId{};
        authoritativeRemoteRestraintReceivedMs_ = 0U;
        previousAuthoritativeRemoteRestraintRagdollMs_ = 0U;
        AI::CLEAR_PED_TASKS_IMMEDIATELY(*handle, FALSE, TRUE);
        Log(
            "[WARNING][RESTRAINT_FSM] remote lasso lease expired; released proxy fail-safe");
    }
    const bool authoritativeRemoteRestraintActive =
        authoritativeRemoteRestraint_ != PlayerRestraintState::Free &&
        authoritativeRemoteRestraintSubject_ == state.entityId;
    if (state.lifecycle != PlayerLifecycle::Alive) {
        if (previousRemoteDownedRagdollMs_ == 0U ||
            now < previousRemoteDownedRagdollMs_ ||
            now - previousRemoteDownedRagdollMs_ >= 700U) {
            (void)PED::SET_PED_TO_RAGDOLL(
                *handle,
                1'250,
                1'250,
                0,
                TRUE,
                TRUE,
                FALSE);
            previousRemoteDownedRagdollMs_ = now;
        }
        ENTITY::FORCE_ENTITY_AI_AND_ANIMATION_UPDATE(*handle, FALSE);
        return true;
    }
    previousRemoteDownedRagdollMs_ = 0U;
    if (authoritativeRemoteRestraintActive) {
        // The local player's real lasso owns this proxy's physical graph.
        // Root correction or a synthetic fall here would detach/obscure the
        // rope. Hold the replicated motor until authoritative Free/lease end.
        ENTITY::FORCE_ENTITY_AI_AND_ANIMATION_UPDATE(*handle, FALSE);
        ++remotePlayerActionMotorYields_;
        return true;
    }

    if (remoteMeleeVisualActionId_ != 0U &&
        remoteMeleeVisualDeadlineMs_ != 0U &&
        now < remoteMeleeVisualDeadlineMs_) {
        // The only native melee call is issued by the reliable Begin handler.
        // Neither motion backend may refresh or replace it before End/deadline.
        ENTITY::FORCE_ENTITY_AI_AND_ANIMATION_UPDATE(*handle, FALSE);
        ++remotePlayerActionMotorYields_;
        return true;
    }

    if (motionReplicationMode_ ==
        MotionReplicationWireMode::AnimGraphReplica) {
        return ApplyRemoteAnimGraphTransform(*handle, state);
    }
    if (reliablePlayerActionProtocolObserved_ &&
        remoteActionLassoActive_ &&
        remotePeerLassoRopeActionId_ != 0U &&
        (remotePeerLassoTaskPending_ ||
         remotePeerLassoEngineOwned_)) {
        ENTITY::FORCE_ENTITY_AI_AND_ANIMATION_UPDATE(*handle, FALSE);
        ++remotePlayerActionMotorYields_;
        return true;
    }

    auto currentPosition = ToBridgeVector(
        ENTITY::GET_ENTITY_COORDS(*handle, TRUE, FALSE));
    const auto currentVelocity = ToBridgeVector(
        ENTITY::GET_ENTITY_VELOCITY(*handle, 0));
    if (!IsFinite(currentPosition) ||
        !IsFinite(currentVelocity)) {
        return false;
    }

    const bool epochCarriesTraversalIntent =
        state.traversalKind != PlayerTraversalKind::None &&
        state.traversalActionId != 0U;
    if (state.locomotionEpoch != 0U &&
        state.locomotionEpoch != remoteLocomotionEpoch_) {
        const bool changedEpoch = remoteLocomotionEpoch_ != 0U;
        remoteLocomotionEpoch_ = state.locomotionEpoch;
        previousRemoteTaskMs_ = 0U;
        previousRemoteNavigationTaskMs_ = 0U;
        previousRemoteAimTaskMs_ = 0U;
        previousRemoteMeleeTaskMs_ = 0U;
        hasRemoteLocomotionTask_ = false;
        // A traversal epoch invalidates interpolation across the action, but
        // the already-buffered route up to its anchor is still exactly what a
        // lagging proxy needs in order to reach the fence/window. Clearing it
        // here made the proxy stop when the authoritative player jumped.
        if (!epochCarriesTraversalIntent) {
            remoteWaypoints_.clear();
        }
        if (!epochCarriesTraversalIntent) {
            pendingRemoteTraversals_.clear();
        }
        remoteNavigationActive_ = false;
        hasRemoteNavigationDestination_ = false;
        remoteNavigationDestinationWaypointCount_ = 0U;
        remoteNavigationStalledSinceMs_ = 0U;
        if (!epochCarriesTraversalIntent) {
            remoteTraversalTaskGuardUntilMs_ = 0U;
        }
        if (changedEpoch) {
            if (!epochCarriesTraversalIntent) {
                ++remoteNavigationTrailResets_;
            }
            Log(
                "[INFO][PUPPET_EPOCH] locomotion epoch changed to " +
                std::to_string(state.locomotionEpoch) +
                (epochCarriesTraversalIntent
                     ? "; traversal approach route preserved"
                     : "; stale route/tasks discarded"));
        }
    }
    const auto rawApplyGap =
        previousRemoteTransformMs_ == 0U ||
                now <= previousRemoteTransformMs_
            ? 0U
            : now - previousRemoteTransformMs_;
    const auto elapsed =
        rawApplyGap == 0U
            ? 50U
            : static_cast<std::uint32_t>(
                  std::clamp<std::uint64_t>(
                      rawApplyGap,
                      1U,
                      250U));
    remoteMotionMaximumApplyGapMs_ = std::max(
        remoteMotionMaximumApplyGapMs_,
        rawApplyGap);
    previousRemoteTransformMs_ = now;

    const bool diagnosticAimTargetChanged =
        hasAimTarget &&
        (lastRemoteAimTargetMs_ == 0U ||
         Distance(lastRemoteAimTarget_, state.aimTarget) >= 0.10F);
    if (diagnosticAimTargetChanged) {
        ++remoteActionAimTargetUpdates_;
    }
    if (hasAimTarget) {
        lastRemoteAimTarget_ = state.aimTarget;
        lastRemoteAimTargetMs_ = now;
    }
    const bool cachedAimTargetFresh =
        lastRemoteAimTargetMs_ != 0U &&
        now >= lastRemoteAimTargetMs_ &&
        now - lastRemoteAimTargetMs_ <=
            kRemotePlayerAimTargetCacheMilliseconds;
    if (state.fireSequence != 0U) {
        const auto fireDisposition =
            remoteFireSequences_.Observe(
                state.fireSequence);
        const bool baselineCarriesShot =
            fireDisposition == SequenceDisposition::First &&
            (state.flags & kFiring) != 0U;
        if ((baselineCarriesShot ||
             fireDisposition == SequenceDisposition::Newer) &&
            (hasAimTarget || cachedAimTargetFresh)) {
            pendingRemoteFireTarget_ =
                hasAimTarget
                    ? state.aimTarget
                    : lastRemoteAimTarget_;
            pendingRemoteFireExpiresMs_ =
                now + kRemotePlayerFireLatchMilliseconds;
            ++remoteActionFireEvents_;
        }
    }

    const bool requestedAiming =
        hasAimTarget &&
        (reliablePlayerActionProtocolObserved_
             ? remoteActionAimActive_
             : (state.flags & kAiming) != 0U) &&
        !(reliablePlayerActionProtocolObserved_
              ? remoteActionMeleeActive_ ||
                    remoteActionBlockActive_ ||
                    remoteActionGrappleActive_
              : (state.flags & kMeleeCombat) != 0U ||
                    (state.flags & kMeleeBlocking) != 0U ||
                    (state.flags & kMeleeGrappling) != 0U) &&
        !remoteReloading_;
    if (requestedAiming != remoteAiming_) {
        if (!requestedAiming) {
            AI::CLEAR_PED_SECONDARY_TASK(*handle);
        }
        remoteAiming_ = requestedAiming;
        previousRemoteAimTaskMs_ = 0U;
        previousRemoteAimTarget_ = {};
        previousRemoteTaskMs_ = 0U;
        hasRemoteLocomotionTask_ = false;
        ++remoteActionAimTransitions_;
    }
    const bool requestedMeleeCombat =
        (reliablePlayerActionProtocolObserved_
             ? remoteActionMeleeActive_ || remoteActionGrappleActive_
             : (state.flags & kMeleeCombat) != 0U ||
                   (state.flags & kMeleeGrappling) != 0U) &&
        !(reliablePlayerActionProtocolObserved_
              ? remoteActionBlockActive_
              : (state.flags & kMeleeBlocking) != 0U) &&
        !remoteReloading_;
    const bool requestedMeleeBlocking =
        reliablePlayerActionProtocolObserved_
            ? remoteActionBlockActive_
            : (state.flags & kMeleeBlocking) != 0U;
    if (requestedMeleeBlocking != remoteMeleeBlocking_) {
        remoteMeleeBlocking_ = requestedMeleeBlocking;
        Log(
            requestedMeleeBlocking
                ? "[INFO][PEER_COMBAT] remote block intent started"
                : "[INFO][PEER_COMBAT] remote block intent released");
    }
    if (requestedMeleeCombat != remoteMeleeCombat_) {
        remoteMeleeCombat_ = requestedMeleeCombat;
        previousRemoteMeleeTaskMs_ = 0U;
        previousRemoteTaskMs_ = 0U;
        hasRemoteLocomotionTask_ = false;
        Log(
            requestedMeleeCombat
                ? "[INFO][PEER_COMBAT] remote melee attack/grapple pulse started"
                : "[INFO][PEER_COMBAT] remote melee attack/grapple pulse released");
    }
    const bool requestedJumping =
        (state.flags & kJumping) != 0U &&
        !replicatedMounted;
    const bool requestedClimbing =
        (state.flags & kClimbing) != 0U &&
        !replicatedMounted;
    const bool semanticTraversal =
        state.traversalKind != PlayerTraversalKind::None &&
        state.traversalActionId != 0U;
    const bool startSemanticTraversal =
        semanticTraversal &&
        state.traversalActionId !=
            lastRemoteTraversalActionId_;
    const bool startRemoteJump =
        startSemanticTraversal
            ? state.traversalKind == PlayerTraversalKind::Jump
            : state.locomotionEpoch == 0U &&
                  requestedJumping && !remoteJumping_;
    const bool startRemoteClimb =
        startSemanticTraversal
            ? state.traversalKind == PlayerTraversalKind::Climb
            : state.locomotionEpoch == 0U &&
                  requestedClimbing && !remoteClimbing_;
    if (startRemoteClimb || startRemoteJump) {
        constexpr std::size_t kMaximumPendingTraversals = 8U;
        if (pendingRemoteTraversals_.size() >=
            kMaximumPendingTraversals) {
            pendingRemoteTraversals_.pop_front();
            ++remoteActionTraversalExpired_;
        }
        const auto traversalPosition =
            startSemanticTraversal
                ? state.traversalAnchor
                : !remoteWaypoints_.empty() &&
                    now >= remoteWaypoints_.back().capturedAtMs &&
                    now - remoteWaypoints_.back().capturedAtMs <= 500U
                    ? remoteWaypoints_.back().position
                    : state.position;
        pendingRemoteTraversals_.push_back(
            RemoteTraversalIntent{
                startRemoteClimb
                    ? RemoteTraversalKind::Climb
                    : RemoteTraversalKind::Jump,
                traversalPosition,
                startSemanticTraversal
                    ? state.traversalHeading
                    : state.heading,
                startSemanticTraversal
                    ? state.traversalActionId
                    : 0U,
                now});
        if (startSemanticTraversal) {
            lastRemoteTraversalActionId_ =
                state.traversalActionId;
        }
        ++remoteActionTraversalDeferred_;
    }
    if (requestedJumping != remoteJumping_ ||
        requestedClimbing != remoteClimbing_) {
        remoteJumping_ = requestedJumping;
        remoteClimbing_ = requestedClimbing;
        previousRemoteTaskMs_ = 0U;
        previousRemoteNavigationTaskMs_ = 0U;
        hasRemoteLocomotionTask_ = false;
    }

    const auto targetGap =
        hasPreviousRemoteTarget_
            ? Distance(
                  previousRemoteTargetPosition_,
                  state.position)
            : 0.0F;
    const bool authoritativeDiscontinuity =
        hasPreviousRemoteTarget_ &&
        std::isfinite(targetGap) &&
        targetGap >= kRemoteMotionSnapDistanceMeters;
    remoteMotionTargetGapMax_ = std::max(
        remoteMotionTargetGapMax_,
        std::isfinite(targetGap) ? targetGap : 0.0F);
    previousRemoteTargetPosition_ = state.position;
    remoteDiagnosticsTargetHeading_ = state.heading;
    hasPreviousRemoteTarget_ = true;

    const auto locomotionAge =
        !hasRemoteLocomotionState_ ||
                previousRemoteLocomotionChangeMs_ == 0U ||
                now < previousRemoteLocomotionChangeMs_
            ? kRemoteMotionMinimumGaitDwellMs
            : static_cast<std::uint32_t>(
                  std::min<std::uint64_t>(
                      now - previousRemoteLocomotionChangeMs_,
                      std::numeric_limits<std::uint32_t>::max()));
    const auto plannerVelocity =
        hasRemoteMotionAssistVelocity_
            ? remoteMotionAssistVelocity_
            : currentVelocity;
    auto step = PlanRemoteMotion(
        RemoteMotionInput{
            currentPosition,
            plannerVelocity,
            state.position,
            state.velocity,
            ENTITY::GET_ENTITY_HEADING(*handle),
            state.heading,
            elapsed,
            previousRemoteLocomotion_,
            locomotionAge,
            hasRemoteLocomotionState_,
            authoritativeDiscontinuity,
            state.movementHeading,
            state.desiredMoveBlend,
            state.locomotionEpoch != 0U});

    if (step.mode == RemoteMotionMode::Hold) {
        return false;
    }

    ++remoteMotionApplyCount_;
    remoteMotionPositionErrorSum_ +=
        static_cast<double>(step.positionErrorMeters);
    remoteMotionPositionErrorMax_ = std::max(
        remoteMotionPositionErrorMax_,
        step.positionErrorMeters);
    if (step.catchUpActive) {
        ++remoteMotionCatchUpTicks_;
    }
    if (step.headingFollowsDestination) {
        ++remoteMotionDestinationHeadingTicks_;
    }

    const bool previousAimRootSuppressed =
        remoteAimRootSuppressed_;
    remoteAimRootSuppressed_ = ShouldSuppressRemoteAimRoot(
        remoteAimRootSuppressed_,
        remoteAiming_,
        replicatedMounted,
        step.positionErrorMeters);
    if (remoteAimRootSuppressed_) {
        ++remoteActionAimRootSuppressedTicks_;
    }
    if (remoteAimRootSuppressed_ !=
        previousAimRootSuppressed) {
        // The primary task changes owner only at hysteresis boundaries:
        // straight locomotion while catching up, combined strafe/aim once
        // the replica is close. Invalidating here gives an immediate clean
        // transition without clearing the ped task graph.
        previousRemoteTaskMs_ = 0U;
        hasRemoteLocomotionTask_ = false;
        ++remoteActionAimRootSuppressionTransitions_;
    }
    const bool physicsInterrupted =
        PED::IS_PED_RAGDOLL(*handle) != FALSE ||
        PED::IS_PED_FALLING(*handle) != FALSE ||
        PED::IS_PED_JUMPING(*handle) != FALSE ||
        PED::IS_PED_CLIMBING(*handle) != FALSE ||
        AI::IS_PED_GETTING_UP(*handle) != FALSE;
    if (physicsInterrupted) {
        ++remoteMotionPhysicsInterruptedTicks_;
    }
    if (physicsInterrupted != remoteMotionPhysicsInterrupted_) {
        remoteMotionPhysicsInterrupted_ = physicsInterrupted;
        previousRemoteTaskMs_ = 0U;
        hasRemoteLocomotionTask_ = false;
        ++remoteMotionPhysicsInterruptionTransitions_;
    }

    if (authoritativeDiscontinuity || replicatedMounted) {
        remoteWaypoints_.clear();
        remoteNavigationStalledSinceMs_ = 0U;
        remoteNavigationEnteredMs_ = 0U;
        remoteNavigationCooldownUntilMs_ = 0U;
        previousRemoteNavigationTaskMs_ = 0U;
        remoteNavigationDestinationWaypointCount_ = 0U;
        hasRemoteNavigationDestination_ = false;
        remoteNavigationActive_ = false;
        hasRemoteNavigationProbe_ = false;
        pendingRemoteTraversals_.clear();
        remoteTraversalTaskGuardUntilMs_ = 0U;
    }
    if (!replicatedMounted) {
        CaptureRemoteWaypoint(state.position, now);
        ConsumeReachedRemoteWaypoints(currentPosition);
        if (!remoteNavigationActive_ && !remoteWaypoints_.empty()) {
            const auto curvature =
                EstimateRemoteRouteCurvatureDegrees();
            const auto lookAhead =
                ComputeRemoteRouteLookAheadMeters(
                    step.locomotion,
                    step.positionErrorMeters,
                    curvature);
            if (const auto routeTarget =
                    SelectRemoteRouteWaypoint(lookAhead);
                routeTarget.has_value()) {
                step.taskDestination = *routeTarget;
            }
            remoteRouteLookAheadSum_ += lookAhead;
            remoteRouteLookAheadMaximum_ = std::max(
                remoteRouteLookAheadMaximum_,
                lookAhead);
            remoteRouteCurvatureMaximum_ = std::max(
                remoteRouteCurvatureMaximum_,
                curvature);
            ++remoteRouteLookAheadSamples_;
        }
    }

    if (!replicatedMounted &&
        (!hasRemoteNavigationProbe_ ||
         now < previousRemoteNavigationProbeMs_ ||
         now - previousRemoteNavigationProbeMs_ >=
             kRemoteNavigationProbeIntervalMs)) {
        if (hasRemoteNavigationProbe_) {
            const auto pedTravel = HorizontalDistance(
                remoteNavigationProbePosition_,
                currentPosition);
            const auto targetTravel = HorizontalDistance(
                remoteNavigationProbeTarget_,
                state.position);
            const auto errorImprovement =
                remoteNavigationProbeError_ -
                step.positionErrorMeters;
            const bool stalledSample =
                IsRemoteNavigationStalledSample(
                    step.positionErrorMeters,
                    pedTravel,
                    targetTravel,
                    errorImprovement);
            if (stalledSample) {
                ++remoteNavigationStalledSamples_;
                if (remoteNavigationStalledSinceMs_ == 0U) {
                    remoteNavigationStalledSinceMs_ = now;
                }
            } else if (
                step.positionErrorMeters <
                    kRemoteNavigationEnterErrorMeters ||
                errorImprovement >=
                    kRemoteNavigationMinimumErrorImprovementMeters ||
                (targetTravel <
                     kRemoteNavigationMinimumTargetTravelMeters &&
                 pedTravel >=
                     kRemoteNavigationMinimumPedTravelMeters)) {
                remoteNavigationStalledSinceMs_ = 0U;
            }
        }
        remoteNavigationProbePosition_ = currentPosition;
        remoteNavigationProbeTarget_ = state.position;
        remoteNavigationProbeError_ = step.positionErrorMeters;
        previousRemoteNavigationProbeMs_ = now;
        hasRemoteNavigationProbe_ = true;
    }

    const auto navigationStalledForMs =
        remoteNavigationStalledSinceMs_ == 0U ||
                now < remoteNavigationStalledSinceMs_
            ? 0U
            : now - remoteNavigationStalledSinceMs_;
    const bool watchdogProtectedTransition =
        physicsInterrupted ||
        replicatedMounted ||
        !pendingRemoteTraversals_.empty() ||
        now < remoteTraversalTaskGuardUntilMs_;
    const bool watchdogCooldownElapsed =
        previousRemoteTaskWatchdogReissueMs_ == 0U ||
        now < previousRemoteTaskWatchdogReissueMs_ ||
        now - previousRemoteTaskWatchdogReissueMs_ >=
            remoteTaskWatchdogCooldownMs_;
    if (!watchdogProtectedTransition &&
        !remoteNavigationActive_ &&
        hasRemoteLocomotionTask_ &&
        navigationStalledForMs >=
            kRemoteTaskWatchdogToleranceMilliseconds &&
        watchdogCooldownElapsed) {
        hasRemoteLocomotionTask_ = false;
        previousRemoteTaskMs_ = 0U;
        ++remoteTaskWatchdogReissues_;
        previousRemoteTaskWatchdogReissueMs_ = now;
        remoteTaskWatchdogCooldownMs_ = std::min<std::uint64_t>(
            kRemoteTaskWatchdogMaximumCooldownMilliseconds,
            remoteTaskWatchdogCooldownMs_ +
                (remoteTaskWatchdogCooldownMs_ / 2U));
    } else if (navigationStalledForMs == 0U) {
        remoteTaskWatchdogCooldownMs_ =
            kRemoteTaskWatchdogInitialCooldownMilliseconds;
        previousRemoteTaskWatchdogReissueMs_ = 0U;
    }

    const bool hardResyncCooldownActive =
        now < remoteHardResyncCooldownUntilMs_;
    const bool hardResyncEligible =
        !hardResyncCooldownActive &&
        !physicsInterrupted &&
        !replicatedMounted &&
        !authoritativeDiscontinuity &&
        step.positionErrorMeters >=
            kRemoteMotionHardResyncDistanceMeters;
    if (hardResyncEligible) {
        if (remoteHardResyncErrorStartedMs_ == 0U ||
            now < remoteHardResyncErrorStartedMs_) {
            remoteHardResyncErrorStartedMs_ = now;
        }
    } else {
        remoteHardResyncErrorStartedMs_ = 0U;
    }
    const auto hardResyncSustainedForMs =
        remoteHardResyncErrorStartedMs_ == 0U ||
                now < remoteHardResyncErrorStartedMs_
            ? 0U
            : now - remoteHardResyncErrorStartedMs_;
    const bool hardResyncRequested = ShouldApplyRemoteHardResync(
        step.positionErrorMeters,
        hardResyncSustainedForMs,
        hardResyncCooldownActive,
        physicsInterrupted,
        replicatedMounted,
        authoritativeDiscontinuity);
    const auto hardResyncPosition =
        hardResyncRequested
            ? GroundSafePosition(state.position)
            : state.position;
    const auto hardResyncCorrectionDistance =
        hardResyncRequested
            ? Distance(currentPosition, hardResyncPosition)
            : 0.0F;
    const bool hardResyncWillApply =
        hardResyncRequested &&
        IsFinite(hardResyncPosition) &&
        std::isfinite(hardResyncCorrectionDistance) &&
        hardResyncCorrectionDistance >=
            kRemoteNavigationSafeRecoveryMinimumDistanceMeters;
    bool proactiveHardResyncApplied{};
    if (hardResyncWillApply) {
        const bool emergency =
            step.positionErrorMeters >=
            kRemoteMotionEmergencyHardResyncDistanceMeters;
        const auto discardedWaypoints = remoteWaypoints_.size();
        const auto discardedTraversals = pendingRemoteTraversals_.size();
        if (remoteNavigationActive_) {
            ++remoteNavigationExits_;
        }
        ENTITY::SET_ENTITY_COORDS_NO_OFFSET(
            *handle,
            hardResyncPosition.x,
            hardResyncPosition.y,
            hardResyncPosition.z,
            FALSE,
            FALSE,
            FALSE);
        ENTITY::SET_ENTITY_VELOCITY(
            *handle,
            0.0F,
            0.0F,
            0.0F);
        ENTITY::SET_ENTITY_HEADING(*handle, state.heading);
        AI::TASK_STAND_STILL(*handle, 100);
        PED::SET_PED_KEEP_TASK(*handle, TRUE);
        currentPosition = hardResyncPosition;
        remoteWaypoints_.clear();
        pendingRemoteTraversals_.clear();
        remoteTraversalTaskGuardUntilMs_ = 0U;
        remoteNavigationActive_ = false;
        remoteNavigationEnteredMs_ = 0U;
        remoteNavigationStalledSinceMs_ = 0U;
        remoteNavigationCooldownUntilMs_ =
            now + kRemoteMotionHardResyncCooldownMs;
        previousRemoteNavigationTaskMs_ = 0U;
        hasRemoteNavigationDestination_ = false;
        remoteNavigationDestinationWaypointCount_ = 0U;
        hasRemoteNavigationProbe_ = false;
        previousRemoteTaskMs_ = 0U;
        hasRemoteLocomotionTask_ = false;
        remoteMotionAssistVelocity_ = {};
        hasRemoteMotionAssistVelocity_ = false;
        remoteMotionPhysicsAssistActive_ = false;
        remoteMotionAppliedMoveRate_ = 1.0F;
        previousRemoteCoordinateCorrectionMs_ = now;
        remoteHardResyncCooldownUntilMs_ =
            now + kRemoteMotionHardResyncCooldownMs;
        remoteHardResyncErrorStartedMs_ = 0U;
        ++remoteMotionSoftCorrections_;
        ++remoteMotionHardResyncs_;
        if (emergency) {
            ++remoteMotionEmergencyHardResyncs_;
        }
        remoteMotionHardResyncMaximumDistance_ = std::max(
            remoteMotionHardResyncMaximumDistance_,
            hardResyncCorrectionDistance);
        remoteMotionHardResyncMaximumWaitMs_ = std::max(
            remoteMotionHardResyncMaximumWaitMs_,
            hardResyncSustainedForMs);
        ++remoteNavigationTrailResets_;
        remoteActionTraversalExpired_ += discardedTraversals;
        proactiveHardResyncApplied = true;
        Log(
            "[WARN][PUPPET_HARD_RESYNC] marker lock corrected " +
            std::to_string(hardResyncCorrectionDistance) +
            " m after " +
            std::to_string(hardResyncSustainedForMs) +
            " ms; emergency=" +
            std::to_string(emergency) +
            ", discarded-waypoints=" +
            std::to_string(discardedWaypoints) +
            ", discarded-traversals=" +
            std::to_string(discardedTraversals));
    }
    const bool navigationWasActive = remoteNavigationActive_;
    const auto navigationActiveForMs =
        !navigationWasActive || remoteNavigationEnteredMs_ == 0U ||
                now < remoteNavigationEnteredMs_
            ? 0U
            : now - remoteNavigationEnteredMs_;
    const bool navigationTimedOut =
        navigationWasActive &&
        HasRemoteNavigationRecoveryTimedOut(
            navigationActiveForMs,
            physicsInterrupted);
    // A timed-out nav task points only ~2 m ahead. Rejoining that old point
    // cannot close a growing sprint backlog. Once the full recovery budget is
    // exhausted, advance to the newest authoritative route point and discard
    // the obsolete prefix in one operation.
    auto safeRecoveryDestination = remoteNavigationDestination_;
    auto safeRecoveryWaypointCount =
        remoteNavigationDestinationWaypointCount_;
    bool safeRecoveryTargetsTraversal{};
    if (navigationTimedOut) {
        std::size_t inspectedWaypointCount{};
        for (const auto& waypoint : remoteWaypoints_) {
            ++inspectedWaypointCount;
            const auto correctionDistance =
                Distance(currentPosition, waypoint.position);
            if (std::isfinite(correctionDistance) &&
                correctionDistance >=
                    kRemoteNavigationSafeRecoveryMinimumDistanceMeters &&
                correctionDistance <=
                    kRemoteNavigationSafeRecoveryMaximumDistanceMeters) {
                // Keep walking forward through the queue and retain the newest
                // point that still satisfies the hard correction bound. If
                // the peer is more than 40 m ahead this still makes bounded
                // progress instead of disabling recovery altogether.
                safeRecoveryDestination = waypoint.position;
                safeRecoveryWaypointCount = inspectedWaypointCount;
            }
        }
    }
    if (navigationTimedOut && !pendingRemoteTraversals_.empty()) {
        const auto& traversal = pendingRemoteTraversals_.front();
        const auto correctionDistance =
            Distance(currentPosition, traversal.position);
        if (std::isfinite(correctionDistance) &&
            correctionDistance >=
                kRemoteNavigationSafeRecoveryMinimumDistanceMeters &&
            correctionDistance <=
                kRemoteNavigationSafeRecoveryMaximumDistanceMeters) {
            safeRecoveryDestination = traversal.position;
            safeRecoveryTargetsTraversal = true;

            // Discard only the route prefix leading to this action. Later
            // points remain available after the deferred climb/jump commits.
            std::size_t inspectedWaypointCount{};
            std::size_t closestWaypointCount{};
            auto closestDistance =
                std::numeric_limits<float>::infinity();
            for (const auto& waypoint : remoteWaypoints_) {
                ++inspectedWaypointCount;
                const auto distanceToAnchor = HorizontalDistance(
                    waypoint.position,
                    traversal.position);
                if (std::isfinite(distanceToAnchor) &&
                    distanceToAnchor <= closestDistance) {
                    closestDistance = distanceToAnchor;
                    closestWaypointCount = inspectedWaypointCount;
                }
            }
            safeRecoveryWaypointCount = closestWaypointCount;
        }
    }
    const bool hasSafeRecoveryDestination =
        safeRecoveryTargetsTraversal ||
        (hasRemoteNavigationDestination_ &&
         safeRecoveryWaypointCount > 0U);
    const auto distanceToSafeRecoveryDestination =
        hasSafeRecoveryDestination
            ? Distance(currentPosition, safeRecoveryDestination)
            : std::numeric_limits<float>::infinity();
    const bool navigationSafeRecoveryRequested =
        ShouldApplyRemoteNavigationSafeRecovery(
            navigationTimedOut,
            hasSafeRecoveryDestination,
            step.positionErrorMeters,
            distanceToSafeRecoveryDestination,
            physicsInterrupted,
            replicatedMounted);
    const auto safeRecoveryPosition =
        navigationSafeRecoveryRequested
            ? GroundSafePosition(safeRecoveryDestination)
            : safeRecoveryDestination;
    const auto safeRecoveryCorrectionDistance =
        navigationSafeRecoveryRequested
            ? Distance(currentPosition, safeRecoveryPosition)
            : std::numeric_limits<float>::infinity();
    const auto safeRecoveryRemainingError =
        navigationSafeRecoveryRequested
            ? Distance(safeRecoveryPosition, state.position)
            : std::numeric_limits<float>::infinity();
    const bool navigationSafeRecoveryWillApply =
        navigationSafeRecoveryRequested &&
        IsFinite(safeRecoveryPosition) &&
        std::isfinite(safeRecoveryCorrectionDistance) &&
        safeRecoveryCorrectionDistance >=
            kRemoteNavigationSafeRecoveryMinimumDistanceMeters &&
        safeRecoveryCorrectionDistance <=
            kRemoteNavigationSafeRecoveryMaximumDistanceMeters;
    const bool navigationCoolingDown =
        !navigationWasActive &&
        now < remoteNavigationCooldownUntilMs_;
    const bool traversalOwnsApproach =
        !pendingRemoteTraversals_.empty() &&
        HorizontalDistance(
            currentPosition,
            pendingRemoteTraversals_.front().position) <=
            kRemoteTraversalApproachDistanceMeters;
    remoteNavigationActive_ =
        !navigationTimedOut &&
        !navigationCoolingDown &&
        !traversalOwnsApproach &&
        ShouldUseRemoteNavigationRecovery(
            navigationWasActive,
            step.positionErrorMeters,
            navigationStalledForMs,
            physicsInterrupted,
            replicatedMounted);
    if (remoteNavigationActive_ != navigationWasActive) {
        previousRemoteTaskMs_ = 0U;
        hasRemoteLocomotionTask_ = false;
        previousRemoteNavigationTaskMs_ = 0U;
        hasRemoteNavigationDestination_ = false;
        remoteNavigationDestinationWaypointCount_ = 0U;
        remoteMotionAssistVelocity_ = {};
        hasRemoteMotionAssistVelocity_ = false;
        remoteMotionPhysicsAssistActive_ = false;
        if (remoteNavigationActive_) {
            remoteNavigationEnteredMs_ = now;
            ++remoteNavigationEntries_;
            Log(
                "[INFO][PUPPET_NAV] entered navmesh recovery: error-m=" +
                std::to_string(step.positionErrorMeters) +
                ", stalled-ms=" +
                std::to_string(navigationStalledForMs) +
                ", queued-waypoints=" +
                std::to_string(remoteWaypoints_.size()));
        } else {
            const auto activeForMs =
                remoteNavigationEnteredMs_ == 0U ||
                        now < remoteNavigationEnteredMs_
                    ? 0U
                    : now - remoteNavigationEnteredMs_;
            const auto discardedWaypoints =
                navigationTimedOut &&
                        navigationSafeRecoveryWillApply
                    ? std::min(
                          safeRecoveryWaypointCount,
                          remoteWaypoints_.size())
                    : remoteWaypoints_.size();
            ++remoteNavigationExits_;
            Log(
                "[INFO][PUPPET_NAV] exited navmesh recovery: error-m=" +
                std::to_string(step.positionErrorMeters) +
                ", active-ms=" +
                std::to_string(activeForMs) +
                ", queued-waypoints=" +
                std::to_string(remoteWaypoints_.size()) +
                ", discarded-waypoints=" +
                std::to_string(discardedWaypoints) +
                ", reason=" +
                (navigationTimedOut ? "timeout" : "caught-up"));
            if (navigationTimedOut) {
                if (navigationSafeRecoveryWillApply) {
                    const auto removeCount = std::min(
                        safeRecoveryWaypointCount,
                        remoteWaypoints_.size());
                    for (std::size_t index = 0U;
                         index < removeCount;
                         ++index) {
                        remoteWaypoints_.pop_front();
                        ++remoteNavigationWaypointReached_;
                    }
                } else {
                    remoteWaypoints_.clear();
                }
                remoteNavigationCooldownUntilMs_ =
                    now + kRemoteNavigationRetryCooldownMs;
                ++remoteNavigationTimeouts_;
                if (!navigationSafeRecoveryWillApply) {
                    ++remoteNavigationTrailResets_;
                }
            } else if (step.positionErrorMeters <=
                kRemoteNavigationExitErrorMeters) {
                remoteWaypoints_.clear();
                ++remoteNavigationTrailResets_;
            }
            remoteNavigationEnteredMs_ = 0U;
            remoteNavigationStalledSinceMs_ = 0U;
        }
    }
    bool navigationSafeRecoveryApplied{};
    if (navigationSafeRecoveryWillApply) {
            ENTITY::SET_ENTITY_COORDS_NO_OFFSET(
                *handle,
                safeRecoveryPosition.x,
                safeRecoveryPosition.y,
                safeRecoveryPosition.z,
                FALSE,
                FALSE,
                FALSE);
            ENTITY::SET_ENTITY_VELOCITY(
                *handle,
                0.0F,
                0.0F,
                0.0F);
            currentPosition = safeRecoveryPosition;
            remoteMotionAssistVelocity_ = {};
            hasRemoteMotionAssistVelocity_ = false;
            remoteMotionPhysicsAssistActive_ = false;
            remoteMotionAppliedMoveRate_ = 1.0F;
            previousRemoteTaskMs_ = 0U;
            hasRemoteLocomotionTask_ = false;
            navigationSafeRecoveryApplied = true;
            ++remoteMotionSoftCorrections_;
            ++remoteNavigationSafeRecoveryTeleports_;
            remoteNavigationSafeRecoveryMaxDistance_ = std::max(
                remoteNavigationSafeRecoveryMaxDistance_,
                safeRecoveryCorrectionDistance);
            Log(
                "[WARN][PUPPET_SAFE_RECOVERY] navmesh timed out; advanced " +
                std::to_string(safeRecoveryCorrectionDistance) +
                " m to the newest bounded route point; remaining-error-m=" +
                std::to_string(safeRecoveryRemainingError) +
                ", discarded-waypoints=" +
                std::to_string(safeRecoveryWaypointCount) +
                ", target=" +
                (safeRecoveryTargetsTraversal
                     ? "traversal-anchor"
                     : "route"));
    }
    if (remoteNavigationActive_) {
        ++remoteNavigationActiveTicks_;
        if (physicsInterrupted) {
            ++remoteNavigationPausedTicks_;
        }
    }

    for (auto iterator = pendingRemoteTraversals_.begin();
         iterator != pendingRemoteTraversals_.end();) {
        const auto age =
            now >= iterator->capturedAtMs
                ? now - iterator->capturedAtMs
                : 0U;
        if (age > kRemoteTraversalMaximumAgeMs) {
            iterator = pendingRemoteTraversals_.erase(iterator);
            ++remoteActionTraversalExpired_;
        } else {
            ++iterator;
        }
    }
    bool startedDeferredTraversal{};
    for (auto iterator = pendingRemoteTraversals_.begin();
         now >= remoteTraversalTaskGuardUntilMs_ &&
         iterator != pendingRemoteTraversals_.end();
         ++iterator) {
        auto& pending = *iterator;
        const auto age =
            now >= pending.capturedAtMs
                ? now - pending.capturedAtMs
                : 0U;
        const auto distanceToAction = HorizontalDistance(
            currentPosition,
            pending.position);
        if (pending.kind == RemoteTraversalKind::Climb &&
            distanceToAction <=
                kRemoteTraversalApproachDistanceMeters &&
            !pending.geometryProbeComplete) {
            if (pending.geometryProbeHandle == 0) {
                const auto headingRadians =
                    pending.heading *
                    (std::numbers::pi_v<float> / 180.0F);
                const Vec3 probeStart{
                    currentPosition.x,
                    currentPosition.y,
                    currentPosition.z + 0.45F};
                Vec3 probeEnd{
                    probeStart.x +
                        (-std::sin(headingRadians) *
                         kLocalTraversalProbeLengthMeters),
                    probeStart.y +
                        (std::cos(headingRadians) *
                         kLocalTraversalProbeLengthMeters),
                    probeStart.z + 0.20F};
                const bool senderObstacleValid =
                    (pending.flags &
                     static_cast<std::uint32_t>(
                         PlayerTraversalFlag::ObstacleValid)) != 0U;
                if (senderObstacleValid &&
                    IsFinite(pending.obstaclePoint)) {
                    probeEnd.x = pending.obstaclePoint.x;
                    probeEnd.y = pending.obstaclePoint.y;
                    probeEnd.z = pending.obstaclePoint.z + 0.25F;
                }
                pending.geometryProbeHandle =
                    SHAPETEST::START_SHAPE_TEST_CAPSULE(
                        probeStart.x,
                        probeStart.y,
                        probeStart.z,
                        probeEnd.x,
                        probeEnd.y,
                        probeEnd.z,
                        kLocalTraversalProbeRadiusMeters,
                        kTraversalShapeTestFlags,
                        *handle,
                        7);
                if (pending.geometryProbeStartedMs == 0U) {
                    pending.geometryProbeStartedMs = now;
                }
            } else {
                BOOL hit{};
                Vector3 hitPoint{};
                Vector3 hitNormal{};
                Entity hitEntity{};
                const auto result = SHAPETEST::GET_SHAPE_TEST_RESULT(
                    pending.geometryProbeHandle,
                    &hit,
                    &hitPoint,
                    &hitNormal,
                    &hitEntity);
                if (result == 2) {
                    pending.geometryProbeHandle = 0;
                    pending.geometryProbeComplete = true;
                    if (hit != FALSE) {
                        const auto localPoint =
                            ToBridgeVector(hitPoint);
                        const bool senderObstacleValid =
                            (pending.flags &
                             static_cast<std::uint32_t>(
                                 PlayerTraversalFlag::ObstacleValid)) != 0U;
                        pending.geometryConfirmed =
                            !senderObstacleValid ||
                            Distance(
                                localPoint,
                                pending.obstaclePoint) <=
                                kRemoteTraversalGeometryToleranceMeters;
                        if (pending.geometryConfirmed) {
                            ++remoteActionTraversalGeometryConfirmed_;
                        }
                    }
                } else if (result == 0) {
                    pending.geometryProbeHandle = 0;
                    pending.geometryProbeComplete = true;
                }
            }
        }
        const bool geometryFallbackReady =
            pending.kind != RemoteTraversalKind::Climb ||
            pending.geometryConfirmed ||
            (pending.geometryProbeStartedMs != 0U &&
             now >= pending.geometryProbeStartedMs &&
             now - pending.geometryProbeStartedMs >=
                 kRemoteTraversalGeometryProbeTimeoutMs);
        if (geometryFallbackReady &&
            ShouldExecuteDeferredRemoteTraversal(
                distanceToAction,
                age,
                physicsInterrupted,
                remoteReloading_)) {
            ENTITY::SET_ENTITY_HEADING(
                *handle,
                pending.heading);
            if (pending.kind == RemoteTraversalKind::Climb) {
                if (!pending.geometryConfirmed) {
                    ++remoteActionTraversalGeometryFallback_;
                }
                AI::TASK_CLIMB(*handle, TRUE);
                ++remoteActionClimbTaskStarts_;
            } else {
                AI::TASK_JUMP(*handle, TRUE);
                ++remoteActionJumpTaskStarts_;
            }
            PED::SET_PED_KEEP_TASK(*handle, TRUE);
            const auto traversalGuardMs =
                pending.kind == RemoteTraversalKind::Climb
                    ? kRemoteTraversalClimbTaskGuardMs
                    : kRemoteTraversalJumpTaskGuardMs;
            pendingRemoteTraversals_.erase(iterator);
            remoteTraversalTaskGuardUntilMs_ =
                now + traversalGuardMs;
            previousRemoteTaskMs_ = 0U;
            previousRemoteNavigationTaskMs_ = 0U;
            hasRemoteLocomotionTask_ = false;
            startedDeferredTraversal = true;
            break;
        }
    }
    const bool replicatedTraversalAction =
        startedDeferredTraversal ||
        now < remoteTraversalTaskGuardUntilMs_;

    const bool traversalApproach =
        !pendingRemoteTraversals_.empty() &&
        HorizontalDistance(
            currentPosition,
            pendingRemoteTraversals_.front().position) <=
            kRemoteTraversalApproachDistanceMeters;
    const auto controlMode = SelectPuppetControlMode(
        state.locomotionMode,
        replicatedMounted,
        remoteNavigationActive_,
        traversalApproach,
        replicatedTraversalAction,
        physicsInterrupted,
        step.mode == RemoteMotionMode::Snap ||
            proactiveHardResyncApplied);
    if (!hasRemoteControlMode_ ||
        controlMode != remoteControlMode_) {
        if (hasRemoteControlMode_) {
            ++remoteControlModeTransitions_;
        }
        remoteControlMode_ = controlMode;
        hasRemoteControlMode_ = true;
        previousRemoteTaskMs_ = 0U;
        hasRemoteLocomotionTask_ = false;
    }
    const auto controlModeIndex =
        static_cast<std::size_t>(controlMode);
    if (controlModeIndex < remoteControlModeTicks_.size()) {
        ++remoteControlModeTicks_[controlModeIndex];
    }
    if (controlMode == PuppetControlMode::TraversalApproach &&
        !pendingRemoteTraversals_.empty()) {
        step.taskDestination =
            pendingRemoteTraversals_.front().position;
        step.heading =
            pendingRemoteTraversals_.front().heading;
        if (step.locomotion == RemoteLocomotion::Idle) {
            step.locomotion = RemoteLocomotion::Walk;
            step.moveBlendRatio = std::max(
                step.moveBlendRatio,
                1.0F);
            step.taskSpeed = std::max(step.taskSpeed, 1.0F);
        }
    }

    const bool useCombinedAimLocomotion =
        controlMode == PuppetControlMode::AimingLocomotion &&
        remoteAiming_ && !remoteAimRootSuppressed_;
    const auto moveRateSeconds =
        static_cast<float>(std::min<std::uint32_t>(elapsed, 50U)) /
        1'000.0F;
    const auto moveRateDelta =
        step.moveRateOverride >= remoteMotionAppliedMoveRate_
            ? kRemoteMoveRateRisePerSecond * moveRateSeconds
            : kRemoteMoveRateFallPerSecond * moveRateSeconds;
    remoteMotionAppliedMoveRate_ = MoveTowards(
        remoteMotionAppliedMoveRate_,
        step.moveRateOverride,
        moveRateDelta);
    remoteMotionMaximumMoveRate_ = std::max(
        remoteMotionMaximumMoveRate_,
        remoteMotionAppliedMoveRate_);
    if (remoteMotionDiagnosticsStartedMs_ == 0U) {
        remoteMotionDiagnosticsStartedMs_ = now;
    }

    const auto previousLocomotion = previousRemoteLocomotion_;
    const bool locomotionWasInitialized =
        hasRemoteLocomotionState_;
    if (!hasRemoteLocomotionState_) {
        previousRemoteLocomotion_ = step.locomotion;
        previousRemoteLocomotionChangeMs_ = now;
        hasRemoteLocomotionState_ = true;
    } else if (step.locomotionChanged) {
        previousRemoteLocomotion_ = step.locomotion;
        previousRemoteLocomotionChangeMs_ = now;
        ++remoteMotionGaitChanges_;
    }

    const auto applyLocomotionAnimation =
        [&](const bool forceRefresh) noexcept {
            if (controlMode == PuppetControlMode::Airborne ||
                controlMode == PuppetControlMode::RagdollOrLasso ||
                controlMode == PuppetControlMode::TraversalCommitted ||
                controlMode == PuppetControlMode::Mounted ||
                controlMode == PuppetControlMode::HardResync) {
                // Ragdoll/fall/lasso owns the pose. The authoritative
                // transform can still converge through velocity below, but
                // no walking task may fight the physical animation.
                return;
            }
            PED::SET_PED_MOVE_RATE_OVERRIDE(
                *handle,
                remoteMotionAppliedMoveRate_);
            // Both natives are transient in RDR2 and therefore have to be
            // refreshed on every applied render sample, not merely when a
            // locomotion task starts. The maximum blend keeps the full sprint
            // animation available while move-rate accelerates catch-up.
            PED::SET_PED_MAX_MOVE_BLEND_RATIO(
                *handle,
                3.0F);
            AI::SET_PED_DESIRED_MOVE_BLEND_RATIO(
                *handle,
                step.moveBlendRatio);

            if (controlMode == PuppetControlMode::NavRecovery) {
                if (remoteReloading_ || remoteMeleeCombat_) {
                    return;
                }
                if (hasRemoteNavigationDestination_ &&
                    HorizontalDistance(
                        currentPosition,
                        remoteNavigationDestination_) <=
                        kRemoteNavigationWaypointReachedMeters) {
                    const auto removeCount = std::min(
                        remoteNavigationDestinationWaypointCount_,
                        remoteWaypoints_.size());
                    for (std::size_t index = 0U;
                         index < removeCount;
                         ++index) {
                        remoteWaypoints_.pop_front();
                        ++remoteNavigationWaypointReached_;
                    }
                    hasRemoteNavigationDestination_ = false;
                    remoteNavigationDestinationWaypointCount_ = 0U;
                    previousRemoteNavigationTaskMs_ = 0U;
                }

                const auto navigationDestinationAgeMs =
                    previousRemoteNavigationTaskMs_ == 0U ||
                            now < previousRemoteNavigationTaskMs_
                        ? std::numeric_limits<std::uint64_t>::max()
                        : now - previousRemoteNavigationTaskMs_;
                const auto destinationToCurrentTarget =
                    hasRemoteNavigationDestination_ &&
                            remoteWaypoints_.empty()
                        ? HorizontalDistance(
                              remoteNavigationDestination_,
                              state.position)
                        : 0.0F;
                if (ShouldRefreshRemoteNavigationDestination(
                        hasRemoteNavigationDestination_,
                        navigationDestinationAgeMs,
                        destinationToCurrentTarget,
                        forceRefresh)) {
                    const auto waypoint =
                        SelectRemoteNavigationWaypoint();
                    const bool directToCurrentTarget =
                        !waypoint.has_value() &&
                        ShouldUseDirectRemoteNavigationTarget(
                            step.positionErrorMeters);
                    auto destination =
                        waypoint.value_or(state.position);
                    if (directToCurrentTarget) {
                        remoteNavigationDestinationWaypointCount_ = 0U;
                        ++remoteNavigationDirectTargetSelections_;
                    } else if (!waypoint.has_value()) {
                        remoteNavigationDestinationWaypointCount_ = 0U;
                    }

                    Vector3 safeDestination{};
                    if (PATHFIND::GET_SAFE_COORD_FOR_PED(
                            destination.x,
                            destination.y,
                            destination.z,
                            TRUE,
                            &safeDestination,
                            0) != FALSE) {
                        const auto projected =
                            ToBridgeVector(safeDestination);
                        if (IsFinite(projected) &&
                            HorizontalDistance(
                                destination,
                                projected) <= 4.0F) {
                            destination = projected;
                            ++remoteNavigationSafeCoordHits_;
                        } else {
                            ++remoteNavigationSafeCoordMisses_;
                        }
                    } else {
                        ++remoteNavigationSafeCoordMisses_;
                    }

                    AI::SET_PED_PATH_CAN_USE_CLIMBOVERS(
                        *handle,
                        TRUE);
                    AI::SET_PED_PATH_CAN_USE_LADDERS(
                        *handle,
                        TRUE);
                    AI::SET_PED_PATH_CAN_DROP_FROM_HEIGHT(
                        *handle,
                        FALSE);
                    AI::SET_PED_PATH_AVOID_FIRE(
                        *handle,
                        TRUE);
                    AI::SET_PED_PATH_PREFER_TO_AVOID_WATER(
                        *handle,
                        TRUE,
                        1.0F);
                    AI::TASK_FOLLOW_NAV_MESH_TO_COORD(
                        *handle,
                        destination.x,
                        destination.y,
                        destination.z,
                        std::clamp(
                            step.taskSpeed *
                                std::max(
                                    remoteMotionAppliedMoveRate_,
                                    1.0F),
                            1.5F,
                            6.0F),
                        12'000,
                        0.50F,
                        TRUE,
                        0.0F);
                    PED::SET_PED_KEEP_TASK(*handle, TRUE);
                    remoteNavigationDestination_ = destination;
                    hasRemoteNavigationDestination_ = true;
                    previousRemoteNavigationTaskMs_ = now;
                    previousRemoteTaskMs_ = now;
                    previousRemoteTaskDestination_ = destination;
                    previousRemoteTaskHeading_ = step.heading;
                    hasRemoteLocomotionTask_ = true;
                    ++remoteNavigationTaskStarts_;
                    ++remoteMotionAnimationTaskStarts_;
                }
                return;
            }

            const bool isMoving =
                step.locomotion != RemoteLocomotion::Idle;
            const bool refreshExpired =
                isMoving &&
                (previousRemoteTaskMs_ == 0U ||
                 now < previousRemoteTaskMs_ ||
                 now - previousRemoteTaskMs_ >=
                     kRemoteTaskRefreshMilliseconds);
            const bool minimumRefreshElapsed =
                previousRemoteTaskMs_ == 0U ||
                now < previousRemoteTaskMs_ ||
                now - previousRemoteTaskMs_ >=
                    kRemoteTaskMinimumRefreshMilliseconds;
            const bool headingChanged =
                isMoving &&
                minimumRefreshElapsed &&
                AbsoluteHeadingDifference(
                     previousRemoteTaskHeading_,
                     step.heading) >=
                     kRemoteTaskHeadingRefreshDegrees;
            const bool destinationChanged =
                isMoving &&
                minimumRefreshElapsed &&
                Distance(
                    previousRemoteTaskDestination_,
                    step.taskDestination) >=
                    kRemoteTaskDestinationRefreshMeters;
            const bool aimTargetChanged =
                isMoving &&
                useCombinedAimLocomotion &&
                minimumRefreshElapsed &&
                Distance(
                    previousRemoteAimTarget_,
                    state.aimTarget) >=
                    kRemotePlayerAimTargetRefreshMeters;
            const bool shouldStartTask =
                ShouldRefreshRemoteLocomotionTask(
                    RemoteLocomotionTaskRefreshInput{
                        forceRefresh,
                        hasRemoteLocomotionTask_,
                        locomotionWasInitialized
                            ? previousLocomotion
                            : step.locomotion,
                        step.locomotion,
                        refreshExpired,
                        headingChanged ||
                            destinationChanged ||
                            aimTargetChanged});
            if (!shouldStartTask ||
                remoteReloading_ ||
                remoteMeleeCombat_ ||
                replicatedMounted) {
                return;
            }

            if (step.locomotion == RemoteLocomotion::Idle) {
                if (remoteAiming_) {
                    AI::TASK_AIM_GUN_AT_COORD(
                        *handle,
                        state.aimTarget.x,
                        state.aimTarget.y,
                        state.aimTarget.z,
                        kRemotePlayerAimTaskDurationMilliseconds,
                        FALSE,
                        FALSE);
                    previousRemoteAimTaskMs_ = now;
                    previousRemoteAimTarget_ =
                        state.aimTarget;
                } else {
                    AI::TASK_STAND_STILL(
                        *handle,
                        kRemoteIdleTaskMilliseconds);
                }
            } else if (useCombinedAimLocomotion) {
                // A standalone aim task replaces locomotion and makes the
                // proxy stop/glitch while the remote player walks. Use the
                // game's combined strafe/aim task only after root motion has
                // caught the authoritative marker. At larger error the
                // straight task below owns direction, while visual
                // zero-damage shots remain handled separately.
                AI::TASK_GO_TO_COORD_WHILE_AIMING_AT_COORD(
                    *handle,
                    step.taskDestination.x,
                    step.taskDestination.y,
                    step.taskDestination.z,
                    state.aimTarget.x,
                    state.aimTarget.y,
                    state.aimTarget.z,
                    step.taskSpeed,
                    FALSE,
                    0.5F,
                    0.5F,
                    TRUE,
                    0,
                    FALSE,
                    0,
                    0);
                previousRemoteAimTaskMs_ = now;
                previousRemoteAimTarget_ =
                    state.aimTarget;
            } else {
                AI::TASK_GO_STRAIGHT_TO_COORD(
                    *handle,
                    step.taskDestination.x,
                    step.taskDestination.y,
                    step.taskDestination.z,
                    step.taskSpeed,
                    kRemoteTaskTimeoutMilliseconds,
                    step.heading,
                    kRemoteTaskStoppingRangeMeters,
                    0);
            }
            PED::SET_PED_KEEP_TASK(*handle, TRUE);
            previousRemoteTaskMs_ = now;
            previousRemoteTaskDestination_ =
                step.taskDestination;
            previousRemoteTaskHeading_ = step.heading;
            hasRemoteLocomotionTask_ = true;
            ++remoteMotionAnimationTaskStarts_;
            if (destinationChanged) {
                ++remoteMotionDestinationRefreshes_;
            }
        };

    if (!replicatedMounted) {
        switch (step.mode) {
        case RemoteMotionMode::Hold:
            return false;
        case RemoteMotionMode::SmoothVelocity: {
            if (navigationSafeRecoveryApplied ||
                proactiveHardResyncApplied) {
                // The next render sample rebuilds locomotion from the new
                // grounded position. Applying an old pre-teleport velocity in
                // this frame would visibly launch the replica away again.
                break;
            }
            // Grounded task locomotion is the sole owner of XY. Applying
            // SET_ENTITY_VELOCITY alongside it produced foot sliding,
            // overshoot and failed vaults. Direct velocity is now restricted
            // to an explicitly replicated airborne/ragdoll state; local
            // collision reactions alone do not get pulled through geometry.
            const bool semanticPhysicsMode =
                state.locomotionMode ==
                    PlayerLocomotionMode::Airborne ||
                state.locomotionMode ==
                    PlayerLocomotionMode::Ragdoll;
            const bool applyPhysicsAssist =
                physicsInterrupted &&
                semanticPhysicsMode &&
                controlMode != PuppetControlMode::NavRecovery &&
                controlMode != PuppetControlMode::TraversalCommitted;
            remoteMotionPhysicsAssistActive_ = applyPhysicsAssist;
            if (!applyPhysicsAssist &&
                step.positionErrorMeters >=
                    kRemoteMotionPhysicsAssistEnterDistanceMeters &&
                !physicsInterrupted) {
                ++remoteMotionGroundedVelocitySuppressions_;
            }
            if (controlMode == PuppetControlMode::NavRecovery) {
                ++remoteNavigationAssistSuppressedTicks_;
            }
            if (applyPhysicsAssist) {
                // Continuous root velocity is proportional to the vector
                // from the visible ped to the authoritative marker and is
                // reapplied because the native task can reset it each frame.
                // Unlike SET_ENTITY_COORDS it cannot teleport the proxy.
                // Reloading and physical interruption may travel with the
                // root. This is important for a lassoed or falling remote
                // body: its X/Y/Z still comes from the authoritative peer
                // instead of independently landing on another ledge.
                // Grounded walking, stairs and climbovers must leave vertical
                // motion to RDR2's locomotion/navmesh controller. Applying the
                // marker's positive Z correction here was the source of the
                // levitating puppet seen in Ghost Route V11.1. XYZ correction
                // remains enabled for an actual ragdoll/fall/lasso state.
                const auto assistVerticalVelocity = step.velocity.z;
                ENTITY::SET_ENTITY_VELOCITY(
                    *handle,
                    step.velocity.x,
                    step.velocity.y,
                    assistVerticalVelocity);
                remoteMotionAssistVelocity_ = {
                    step.velocity.x,
                    step.velocity.y,
                    assistVerticalVelocity};
                hasRemoteMotionAssistVelocity_ = true;
                ++remoteMotionPhysicsAssistTicks_;
                if (std::abs(assistVerticalVelocity) >= 0.10F) {
                    ++remoteMotionVerticalAssistTicks_;
                }
                remoteMotionMaximumAssistSpeed_ = std::max(
                    remoteMotionMaximumAssistSpeed_,
                    std::hypot(step.velocity.x, step.velocity.y));
            } else {
                remoteMotionAssistVelocity_ = {};
                hasRemoteMotionAssistVelocity_ = false;
            }
            const bool taskRecoveryCooldownElapsed =
                previousRemoteTaskRecoveryMs_ == 0U ||
                now < previousRemoteTaskRecoveryMs_ ||
                now - previousRemoteTaskRecoveryMs_ >=
                    kRemoteTaskRecoveryCooldownMilliseconds;
            const bool shouldRecoverTask =
                step.positionErrorMeters >=
                    kRemoteTaskRecoveryMeters &&
                taskRecoveryCooldownElapsed &&
                !physicsInterrupted &&
                !remoteNavigationActive_ &&
                !replicatedTraversalAction;
            if (shouldRecoverTask) {
                // Replace the semantic destination without clearing the task
                // graph or moving the ped. CLEAR_PED_TASKS_IMMEDIATELY was
                // the source of the visible rebase/T-pose flash in V7.
                previousRemoteTaskMs_ = 0U;
                previousRemoteTaskDestination_ = {};
                previousRemoteTaskHeading_ = step.heading;
                hasRemoteLocomotionTask_ = false;
                previousRemoteAimTaskMs_ = 0U;
                previousRemoteMeleeTaskMs_ = 0U;
                previousRemoteTaskRecoveryMs_ = now;
                ++remoteMotionTaskRecoveries_;
                applyLocomotionAnimation(true);
                Log(
                    "player puppet v8 task recovery at " +
                    std::to_string(
                        step.positionErrorMeters) +
                    " m local drift; no coordinate snap");
            }
            if (step.locomotion == RemoteLocomotion::Idle &&
                !remoteNavigationActive_ &&
                !replicatedTraversalAction) {
                ENTITY::SET_ENTITY_HEADING(
                    *handle,
                    step.heading);
            }
            applyLocomotionAnimation(false);
            break;
        }
        case RemoteMotionMode::Snap: {
            const auto safePosition =
                GroundSafePosition(step.snapPosition);
            ENTITY::SET_ENTITY_COORDS_NO_OFFSET(
                *handle,
                safePosition.x,
                safePosition.y,
                safePosition.z,
                FALSE,
                FALSE,
                FALSE);
            ENTITY::SET_ENTITY_VELOCITY(
                *handle,
                0.0F,
                0.0F,
                0.0F);
            ENTITY::SET_ENTITY_HEADING(*handle, step.heading);
            remoteMotionAssistVelocity_ = {};
            hasRemoteMotionAssistVelocity_ = false;
            remoteMotionPhysicsAssistActive_ = false;
            remoteMotionAppliedMoveRate_ = 1.0F;
            applyLocomotionAnimation(true);
            previousRemoteCoordinateCorrectionMs_ = now;
            ++remoteMotionWarps_;
            Log(
                "motion v7 warp: authoritative target jumped " +
                std::to_string(targetGap) +
                " m; local error was " +
                std::to_string(step.positionErrorMeters) +
                " m");
            break;
        }
        }
    }
    if (replicatedMounted) {
        remoteMotionAssistVelocity_ = {};
        hasRemoteMotionAssistVelocity_ = false;
        remoteMotionPhysicsAssistActive_ = false;
        remoteMotionAppliedMoveRate_ = 1.0F;
    }

    if (syntheticTest) {
        float targetScreenX{};
        float targetScreenY{};
        float puppetScreenX{};
        float puppetScreenY{};
        const bool targetOnScreen =
            GRAPHICS::GET_SCREEN_COORD_FROM_WORLD_COORD(
                state.position.x,
                state.position.y,
                state.position.z + 0.30F,
                &targetScreenX,
                &targetScreenY) != FALSE;
        const bool puppetOnScreen =
            GRAPHICS::GET_SCREEN_COORD_FROM_WORLD_COORD(
                currentPosition.x,
                currentPosition.y,
                currentPosition.z + 0.90F,
                &puppetScreenX,
                &puppetScreenY) != FALSE;
        if (targetOnScreen) {
            int red = 72;
            int green = 205;
            int blue = 118;
            if (step.positionErrorMeters > 2.0F) {
                red = 220;
                green = 56;
                blue = 42;
            } else if (step.positionErrorMeters > 0.50F) {
                red = 232;
                green = 176;
                blue = 46;
            }
            if (puppetOnScreen) {
                for (int marker = 1; marker <= 8; ++marker) {
                    const auto amount =
                        static_cast<float>(marker) / 9.0F;
                    DrawNativeRectangle(
                        puppetScreenX +
                            ((targetScreenX - puppetScreenX) * amount),
                        puppetScreenY +
                            ((targetScreenY - puppetScreenY) * amount),
                        0.0035F,
                        0.0035F,
                        red,
                        green,
                        blue,
                        150);
                }
            }
            DrawNativeRectangle(
                targetScreenX,
                targetScreenY,
                0.030F,
                0.0030F,
                red,
                green,
                blue,
                235);
            DrawNativeRectangle(
                targetScreenX,
                targetScreenY,
                0.0030F,
                0.052F,
                red,
                green,
                blue,
                235);
            char markerText[64]{};
            std::snprintf(
                markerText,
                sizeof(markerText),
                "NETWORK TARGET  %.1f m",
                static_cast<double>(step.positionErrorMeters));
            DrawNativeText(
                markerText,
                targetScreenX,
                targetScreenY - 0.034F,
                0.24F,
                red,
                green,
                blue,
                245,
                true);
        }
        if (remoteNavigationActive_ &&
            hasRemoteNavigationDestination_) {
            float navigationScreenX{};
            float navigationScreenY{};
            if (GRAPHICS::GET_SCREEN_COORD_FROM_WORLD_COORD(
                    remoteNavigationDestination_.x,
                    remoteNavigationDestination_.y,
                    remoteNavigationDestination_.z + 0.45F,
                    &navigationScreenX,
                    &navigationScreenY) != FALSE) {
                DrawNativeRectangle(
                    navigationScreenX,
                    navigationScreenY,
                    0.024F,
                    0.0025F,
                    66,
                    176,
                    232,
                    235);
                DrawNativeRectangle(
                    navigationScreenX,
                    navigationScreenY,
                    0.0025F,
                    0.042F,
                    66,
                    176,
                    232,
                    235);
                DrawNativeText(
                    "NAVMESH TARGET",
                    navigationScreenX,
                    navigationScreenY - 0.028F,
                    0.22F,
                    110,
                    205,
                    245,
                    245,
                    true);
            }
        }
    }

    if (controlMode == PuppetControlMode::AimingLocomotion &&
        remoteAiming_ &&
        step.locomotion == RemoteLocomotion::Idle) {
        const bool aimRefreshExpired =
            previousRemoteAimTaskMs_ == 0U ||
            now < previousRemoteAimTaskMs_ ||
            now - previousRemoteAimTaskMs_ >=
                kRemotePlayerAimTaskRefreshMilliseconds;
        const auto aimTargetChanged =
            Distance(
                previousRemoteAimTarget_,
                state.aimTarget) >=
            kRemotePlayerAimTargetRefreshMeters;
        if (aimRefreshExpired || aimTargetChanged) {
            AI::TASK_AIM_GUN_AT_COORD(
                *handle,
                state.aimTarget.x,
                state.aimTarget.y,
                state.aimTarget.z,
                kRemotePlayerAimTaskDurationMilliseconds,
                FALSE,
                FALSE);
            PED::SET_PED_KEEP_TASK(*handle, TRUE);
            previousRemoteAimTaskMs_ = now;
            previousRemoteAimTarget_ = state.aimTarget;
        }
    }

    if (pendingRemoteFireExpiresMs_ != 0U &&
        now > pendingRemoteFireExpiresMs_) {
        pendingRemoteFireExpiresMs_ = 0U;
        pendingRemoteFireTarget_ = {};
    }
    const bool weaponActionInterrupted =
        PED::IS_PED_RAGDOLL(*handle) != FALSE ||
        PED::IS_PED_FALLING(*handle) != FALSE ||
        AI::IS_PED_GETTING_UP(*handle) != FALSE;
    if (pendingRemoteFireExpiresMs_ != 0U &&
        remoteWeaponHash_ != 0U &&
        !weaponActionInterrupted) {
        const auto source =
            ToBridgeVector(
                ENTITY::GET_ENTITY_COORDS(
                    *handle,
                    TRUE,
                    FALSE));
        GAMEPLAY::SHOOT_SINGLE_BULLET_BETWEEN_COORDS(
            source.x,
            source.y,
            source.z + 1.0F,
            pendingRemoteFireTarget_.x,
            pendingRemoteFireTarget_.y,
            pendingRemoteFireTarget_.z,
            0,
            TRUE,
            static_cast<Hash>(remoteWeaponHash_),
            *handle,
            TRUE,
            FALSE,
            500.0F,
            FALSE);
        ++remoteActionVisualShots_;
        if (EnsureRemoteGunshotAudio()) {
            AUDIO::_0x6FB1DA3CA9DA7D90(
                reinterpret_cast<Any*>(
                    kRemoteGunshotSoundName),
                static_cast<Any>(*handle),
                reinterpret_cast<Any*>(
                    kRemoteGunshotSoundSet),
                FALSE,
                0,
                0);
            ++remoteActionAudioShots_;
        } else {
            ++remoteActionAudioNotReady_;
        }
        pendingRemoteFireExpiresMs_ = 0U;
        pendingRemoteFireTarget_ = {};
    }

    if (now >= remoteMotionDiagnosticsStartedMs_ &&
        now - remoteMotionDiagnosticsStartedMs_ >=
            kRemoteMotionDiagnosticsMilliseconds) {
        const auto averageError =
            remoteMotionApplyCount_ == 0U
                ? 0.0
                : remoteMotionPositionErrorSum_ /
                      static_cast<double>(
                          remoteMotionApplyCount_);
        Log(
            "[INFO][PUPPET_MOTION] v13.1/marker-lock/5s: ticks=" +
            std::to_string(remoteMotionApplyCount_) +
            ", mean-error-m=" +
            std::to_string(averageError) +
            ", max-error-m=" +
            std::to_string(remoteMotionPositionErrorMax_) +
            ", target-gap-max-m=" +
            std::to_string(remoteMotionTargetGapMax_) +
            ", apply-gap-max-ms=" +
            std::to_string(remoteMotionMaximumApplyGapMs_) +
            ", gait=" +
            std::string{
                RemoteLocomotionName(
                    previousRemoteLocomotion_)} +
            ", gait-changes=" +
            std::to_string(remoteMotionGaitChanges_) +
            ", warps=" +
            std::to_string(remoteMotionWarps_) +
            ", task-recoveries=" +
            std::to_string(remoteMotionTaskRecoveries_) +
            ", watchdog-reissues=" +
            std::to_string(remoteTaskWatchdogReissues_) +
            ", watchdog-cooldown-ms=" +
            std::to_string(remoteTaskWatchdogCooldownMs_) +
            ", animation-task-starts=" +
            std::to_string(
                remoteMotionAnimationTaskStarts_) +
            ", destination-refreshes=" +
            std::to_string(
                remoteMotionDestinationRefreshes_) +
            ", catch-up-ticks=" +
            std::to_string(remoteMotionCatchUpTicks_) +
            ", destination-heading-ticks=" +
            std::to_string(
                remoteMotionDestinationHeadingTicks_) +
            ", move-rate-max=" +
            std::to_string(remoteMotionMaximumMoveRate_) +
            ", physics-assist-ticks=" +
            std::to_string(remoteMotionPhysicsAssistTicks_) +
            ", grounded-velocity-suppressed=" +
            std::to_string(
                remoteMotionGroundedVelocitySuppressions_) +
            ", vertical-assist-ticks=" +
            std::to_string(remoteMotionVerticalAssistTicks_) +
            ", grounded-vertical-suppressions=" +
            std::to_string(remoteMotionGroundedVerticalSuppressions_) +
            ", physics-assist-active=" +
            std::to_string(remoteMotionPhysicsAssistActive_) +
            ", physical-interruption-ticks=" +
            std::to_string(
                remoteMotionPhysicsInterruptedTicks_) +
            ", physical-interruption-transitions=" +
            std::to_string(
                remoteMotionPhysicsInterruptionTransitions_) +
            ", assist-speed-max=" +
            std::to_string(remoteMotionMaximumAssistSpeed_) +
            ", move-rate-per-frame=1, move-rate-range=0.90-" +
            std::to_string(kRemoteMotionCatchUpMaximumMoveRate) +
            ", max-blend-per-frame=3.0" +
            ", controller-mode=" +
            std::string{PuppetControlModeName(remoteControlMode_)} +
            ", controller-transitions=" +
            std::to_string(remoteControlModeTransitions_) +
            ", mode-grounded=" +
            std::to_string(remoteControlModeTicks_[0U]) +
            ", mode-aiming=" +
            std::to_string(remoteControlModeTicks_[1U]) +
            ", mode-traversal-approach=" +
            std::to_string(remoteControlModeTicks_[2U]) +
            ", mode-traversal-committed=" +
            std::to_string(remoteControlModeTicks_[3U]) +
            ", mode-airborne=" +
            std::to_string(remoteControlModeTicks_[4U]) +
            ", mode-ragdoll=" +
            std::to_string(remoteControlModeTicks_[5U]) +
            ", mode-mounted=" +
            std::to_string(remoteControlModeTicks_[6U]) +
            ", mode-nav=" +
            std::to_string(remoteControlModeTicks_[7U]) +
            ", mode-hard-resync=" +
            std::to_string(remoteControlModeTicks_[8U]) +
            ", visible=" +
            std::to_string(
                ENTITY::IS_ENTITY_VISIBLE(*handle) != FALSE) +
            ", visible-to-script=" +
            std::to_string(
                ENTITY::IS_ENTITY_VISIBLE_TO_SCRIPT(*handle) != FALSE) +
            ", alpha=" +
            std::to_string(ENTITY::GET_ENTITY_ALPHA(*handle)) +
            ", coordinate-corrections=" +
            std::to_string(remoteMotionSoftCorrections_) +
            ", hard-resyncs=" +
            std::to_string(remoteMotionHardResyncs_) +
            ", emergency-hard-resyncs=" +
            std::to_string(remoteMotionEmergencyHardResyncs_) +
            ", hard-resync-max-m=" +
            std::to_string(remoteMotionHardResyncMaximumDistance_) +
            ", hard-resync-max-wait-ms=" +
            std::to_string(remoteMotionHardResyncMaximumWaitMs_) +
            ", nameplate-fallback=" +
            std::to_string(!remoteNickname_.empty()) +
            ", solo-marker=" +
            std::to_string(syntheticTest));
        Log(
            "[INFO][PUPPET_NAV] v13.1/marker-lock/5s: active=" +
            std::to_string(remoteNavigationActive_) +
            ", entries=" +
            std::to_string(remoteNavigationEntries_) +
            ", exits=" +
            std::to_string(remoteNavigationExits_) +
            ", stalled-samples=" +
            std::to_string(remoteNavigationStalledSamples_) +
            ", active-ticks=" +
            std::to_string(remoteNavigationActiveTicks_) +
            ", task-starts=" +
            std::to_string(remoteNavigationTaskStarts_) +
            ", queue=" +
            std::to_string(remoteWaypoints_.size()) +
            ", queue-max=" +
            std::to_string(remoteNavigationMaximumQueue_) +
            ", waypoint-captures=" +
            std::to_string(remoteNavigationWaypointCaptures_) +
            ", waypoint-reached=" +
            std::to_string(remoteNavigationWaypointReached_) +
            ", waypoint-drops=" +
            std::to_string(remoteNavigationWaypointDrops_) +
            ", waypoint-expired=" +
            std::to_string(remoteNavigationExpiredWaypoints_) +
            ", waypoint-obsolete=" +
            std::to_string(remoteNavigationObsoleteWaypoints_) +
            ", trail-resets=" +
            std::to_string(remoteNavigationTrailResets_) +
            ", recovery-timeouts=" +
            std::to_string(remoteNavigationTimeouts_) +
            ", safe-recovery-teleports=" +
            std::to_string(remoteNavigationSafeRecoveryTeleports_) +
            ", safe-recovery-max-m=" +
            std::to_string(remoteNavigationSafeRecoveryMaxDistance_) +
            ", direct-target-selections=" +
            std::to_string(remoteNavigationDirectTargetSelections_) +
            ", physics-paused-ticks=" +
            std::to_string(remoteNavigationPausedTicks_) +
            ", safe-coord-hits=" +
            std::to_string(remoteNavigationSafeCoordHits_) +
            ", safe-coord-misses=" +
            std::to_string(remoteNavigationSafeCoordMisses_) +
            ", assist-suppressed-ticks=" +
            std::to_string(remoteNavigationAssistSuppressedTicks_) +
            ", destination-valid=" +
            std::to_string(hasRemoteNavigationDestination_) +
            ", stalled-for-ms=" +
            std::to_string(navigationStalledForMs) +
            ", route-lookahead-mean-m=" +
            std::to_string(
                remoteRouteLookAheadSamples_ == 0U
                    ? 0.0
                    : remoteRouteLookAheadSum_ /
                          static_cast<double>(remoteRouteLookAheadSamples_)) +
            ", route-lookahead-max-m=" +
            std::to_string(remoteRouteLookAheadMaximum_) +
            ", route-curvature-max-deg=" +
            std::to_string(remoteRouteCurvatureMaximum_) +
            ", locomotion-epoch=" +
            std::to_string(remoteLocomotionEpoch_));
        remoteMotionDiagnosticsStartedMs_ = now;
        remoteMotionApplyCount_ = 0U;
        remoteMotionMaximumApplyGapMs_ = 0U;
        remoteMotionGaitChanges_ = 0U;
        remoteMotionWarps_ = 0U;
        remoteMotionSoftCorrections_ = 0U;
        remoteMotionHardResyncs_ = 0U;
        remoteMotionEmergencyHardResyncs_ = 0U;
        remoteMotionHardResyncMaximumWaitMs_ = 0U;
        remoteMotionHardResyncMaximumDistance_ = 0.0F;
        remoteMotionTaskRecoveries_ = 0U;
        remoteMotionAnimationTaskStarts_ = 0U;
        remoteMotionDestinationRefreshes_ = 0U;
        remoteMotionCatchUpTicks_ = 0U;
        remoteMotionDestinationHeadingTicks_ = 0U;
        remoteMotionPhysicsAssistTicks_ = 0U;
        remoteMotionVerticalAssistTicks_ = 0U;
        remoteMotionGroundedVerticalSuppressions_ = 0U;
        remoteMotionGroundedVelocitySuppressions_ = 0U;
        remoteMotionPhysicsInterruptedTicks_ = 0U;
        remoteMotionPhysicsInterruptionTransitions_ = 0U;
        remoteMotionPositionErrorSum_ = 0.0;
        remoteMotionPositionErrorMax_ = 0.0F;
        remoteMotionTargetGapMax_ = 0.0F;
        remoteMotionMaximumMoveRate_ = 1.0F;
        remoteMotionMaximumAssistSpeed_ = 0.0F;
        remoteControlModeTicks_.fill(0U);
        remoteControlModeTransitions_ = 0U;
        remoteRouteLookAheadSum_ = 0.0;
        remoteRouteLookAheadMaximum_ = 0.0F;
        remoteRouteCurvatureMaximum_ = 0.0F;
        remoteRouteLookAheadSamples_ = 0U;
        remoteTaskWatchdogReissues_ = 0U;
        remoteNavigationEntries_ = 0U;
        remoteNavigationExits_ = 0U;
        remoteNavigationStalledSamples_ = 0U;
        remoteNavigationActiveTicks_ = 0U;
        remoteNavigationTaskStarts_ = 0U;
        remoteNavigationWaypointCaptures_ = 0U;
        remoteNavigationWaypointReached_ = 0U;
        remoteNavigationWaypointDrops_ = 0U;
        remoteNavigationExpiredWaypoints_ = 0U;
        remoteNavigationObsoleteWaypoints_ = 0U;
        remoteNavigationTrailResets_ = 0U;
        remoteNavigationTimeouts_ = 0U;
        remoteNavigationSafeRecoveryTeleports_ = 0U;
        remoteNavigationSafeRecoveryMaxDistance_ = 0.0F;
        remoteNavigationDirectTargetSelections_ = 0U;
        remoteNavigationPausedTicks_ = 0U;
        remoteNavigationSafeCoordHits_ = 0U;
        remoteNavigationSafeCoordMisses_ = 0U;
        remoteNavigationAssistSuppressedTicks_ = 0U;
        remoteNavigationMaximumQueue_ = remoteWaypoints_.size();
    }
    if (remoteActionDiagnosticsStartedMs_ == 0U) {
        remoteActionDiagnosticsStartedMs_ = now;
    }
    if (now >= remoteActionDiagnosticsStartedMs_ &&
        now - remoteActionDiagnosticsStartedMs_ >=
            kRemoteMotionDiagnosticsMilliseconds) {
        Log(
            "[INFO][PUPPET_ACTION] v13.1/reliable-traversal/5s: aiming=" +
            std::to_string(remoteAiming_) +
            ", aim-transitions=" +
            std::to_string(remoteActionAimTransitions_) +
            ", aim-target-updates=" +
            std::to_string(remoteActionAimTargetUpdates_) +
            ", fire-events=" +
            std::to_string(remoteActionFireEvents_) +
            ", visual-zero-damage-shots=" +
            std::to_string(remoteActionVisualShots_) +
            ", spatial-audio-shots=" +
            std::to_string(remoteActionAudioShots_) +
            ", audio-not-ready=" +
            std::to_string(remoteActionAudioNotReady_) +
            ", equipment-updates=" +
            std::to_string(remoteActionEquipmentUpdates_) +
            ", weapon-grants=" +
            std::to_string(remoteActionWeaponGrants_) +
            ", equipment-suppressed=" +
            std::to_string(remoteActionEquipmentSuppressed_) +
            ", aim-root-suppressed=" +
            std::to_string(remoteAimRootSuppressed_) +
            ", aim-root-suppressed-ticks=" +
            std::to_string(
                remoteActionAimRootSuppressedTicks_) +
            ", aim-root-transitions=" +
            std::to_string(
                remoteActionAimRootSuppressionTransitions_) +
            ", jumping=" +
            std::to_string(remoteJumping_) +
            ", climbing=" +
            std::to_string(remoteClimbing_) +
            ", jump-task-starts=" +
            std::to_string(remoteActionJumpTaskStarts_) +
            ", climb-task-starts=" +
            std::to_string(remoteActionClimbTaskStarts_) +
            ", traversal-pending=" +
            std::to_string(pendingRemoteTraversals_.size()) +
            ", traversal-deferred=" +
            std::to_string(remoteActionTraversalDeferred_) +
            ", traversal-expired=" +
            std::to_string(remoteActionTraversalExpired_) +
            ", traversal-reliable-updates=" +
            std::to_string(remoteActionTraversalReliableUpdates_) +
            ", traversal-geometry-confirmed=" +
            std::to_string(remoteActionTraversalGeometryConfirmed_) +
            ", traversal-geometry-fallback=" +
            std::to_string(remoteActionTraversalGeometryFallback_) +
            ", traversal-last-id=" +
            std::to_string(lastRemoteTraversalActionId_) +
            ", weapon-hash=" +
            std::to_string(remoteWeaponHash_) +
            ", ammo=" +
            std::to_string(remoteWeaponAmmo_) +
            ", reloading=" +
            std::to_string(remoteReloading_));
        remoteActionDiagnosticsStartedMs_ = now;
        remoteActionAimTransitions_ = 0U;
        remoteActionAimTargetUpdates_ = 0U;
        remoteActionFireEvents_ = 0U;
        remoteActionVisualShots_ = 0U;
        remoteActionAudioShots_ = 0U;
        remoteActionAudioNotReady_ = 0U;
        remoteActionEquipmentUpdates_ = 0U;
        remoteActionWeaponGrants_ = 0U;
        remoteActionEquipmentSuppressed_ = 0U;
        remoteActionAimRootSuppressedTicks_ = 0U;
        remoteActionAimRootSuppressionTransitions_ = 0U;
        remoteActionJumpTaskStarts_ = 0U;
        remoteActionClimbTaskStarts_ = 0U;
        remoteActionTraversalDeferred_ = 0U;
        remoteActionTraversalExpired_ = 0U;
        remoteActionTraversalReliableUpdates_ = 0U;
        remoteActionTraversalGeometryConfirmed_ = 0U;
        remoteActionTraversalGeometryFallback_ = 0U;
    }
    return true;
#else
    (void)state;
#endif
    return false;
}

bool ScriptHookSdkFacade::ApplyRemoteIdentity(
    const PlayerIdentityPayload& identity) noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        if (!identity.entityId.IsValid() ||
            identity.entityId != remotePlayerId_ ||
            identity.nickname.empty() ||
            identity.nickname.size() >
                kMaximumPlayerNicknameUtf8Bytes) {
            return false;
        }
        const auto handle =
            replicas_.FindLocal(identity.entityId);
        if (!handle.has_value() ||
            ENTITY::DOES_ENTITY_EXIST(*handle) == FALSE) {
            return false;
        }

        remoteNickname_ = identity.nickname;
        if (remoteBlip_ == 0 ||
            RADAR::DOES_BLIP_EXIST(
                static_cast<Blip>(remoteBlip_)) == FALSE) {
            char friendlyStyle[] = "BLIP_STYLE_FRIENDLY";
            const auto style =
                GAMEPLAY::GET_HASH_KEY(friendlyStyle);
            remoteBlip_ = static_cast<int>(
                RADAR::_0x23F74C2FDA6E7C61(
                    style,
                    *handle));
            if (remoteBlip_ != 0) {
                RADAR::SET_BLIP_SCALE(
                    static_cast<Blip>(remoteBlip_),
                    0.85F);
            }
        }
        Log(
            "[INFO][PLAYER_IDENTITY] nickname=" + remoteNickname_ +
            ", stable-nameplate=1" +
            ", minimap-blip=" +
            std::to_string(remoteBlip_ != 0));
        return true;
#else
        (void)identity;
#endif
    } catch (...) {
        // Identity decoration is cosmetic and must never crash the bridge.
    }
    return false;
}

bool ScriptHookSdkFacade::ApplyRemoteAppearance(
    const PlayerAppearanceStatePayload& appearance) noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        constexpr auto kKnownFlags =
            static_cast<std::uint16_t>(
                PlayerAppearanceStateFlag::CompleteComponentSet) |
            static_cast<std::uint16_t>(
                PlayerAppearanceStateFlag::StoryMetaPed);
        if (!appearance.entityId.IsValid() ||
            appearance.entityId != remotePlayerId_ ||
            appearance.schemaVersion != 1U ||
            appearance.revision == 0U ||
            appearance.modelHash == 0U ||
            appearance.fingerprint == 0U ||
            appearance.componentHashes.empty() ||
            appearance.componentHashes.size() >
                kMetaPedMaximumShopComponents ||
            (appearance.flags & ~kKnownFlags) != 0U) {
            return false;
        }
        const auto handle = replicas_.FindLocal(appearance.entityId);
        if (!handle.has_value() ||
            ENTITY::DOES_ENTITY_EXIST(*handle) == FALSE) {
            return false;
        }
        const auto localModelHash = static_cast<std::uint32_t>(
            ENTITY::GET_ENTITY_MODEL(*handle));
        if (localModelHash != appearance.modelHash) {
            Log(
                "[METAPED_APPEARANCE][WAIT_MODEL] remote player model differs from host; refusing a cross-model component reset");
            return false;
        }
        if (remoteAppearanceFingerprint_ == appearance.fingerprint &&
            remoteAppearanceModelHash_ == appearance.modelHash &&
            remoteAppearanceComponents_ == appearance.componentHashes) {
            return true;
        }
        std::unordered_set<std::uint32_t> unique;
        for (const auto component : appearance.componentHashes) {
            if (component == 0U || !unique.insert(component).second) {
                return false;
            }
        }

        const auto ped = static_cast<Ped>(*handle);
        ResetPedComponents(ped);
        for (const auto component : appearance.componentHashes) {
            ApplyShopItemToPed(
                ped,
                static_cast<Hash>(component));
        }
        UpdatePedVariation(ped);
        remoteAppearanceFingerprint_ = appearance.fingerprint;
        remoteAppearanceModelHash_ = appearance.modelHash;
        remoteAppearanceComponents_ = appearance.componentHashes;
        Log(
            "[METAPED_APPEARANCE][APPLIED] model=" +
            std::to_string(appearance.modelHash) +
            ", components=" +
            std::to_string(appearance.componentHashes.size()) +
            ", fingerprint=" +
            std::to_string(appearance.fingerprint));
        return true;
#else
        (void)appearance;
#endif
    } catch (...) {
        Log(
            "[ERROR][METAPED_APPEARANCE][APPLY] native component application failed safely");
    }
    return false;
}

bool ScriptHookSdkFacade::ApplyWorldState(
    const WorldStatePayload& state) noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        constexpr auto kWeatherValid =
            static_cast<std::uint8_t>(
                WorldStateFlag::WeatherValid);
        if (state.hour > 23U ||
            state.minute > 59U ||
            state.second > 59U ||
            state.day < 1U ||
            state.day > 31U ||
            state.month > 11U ||
            state.year < 1800U ||
            state.year > 2200U ||
            (state.flags & ~kWeatherValid) != 0U ||
            !std::isfinite(state.weatherBlend) ||
            state.weatherBlend < 0.0F ||
            state.weatherBlend > 1.0F) {
            return false;
        }

        const auto currentHour = TIME::GET_CLOCK_HOURS();
        const auto currentMinute = TIME::GET_CLOCK_MINUTES();
        const auto currentSecond = TIME::GET_CLOCK_SECONDS();
        const bool currentClockValid =
            currentHour >= 0 && currentHour <= 23 &&
            currentMinute >= 0 && currentMinute <= 59 &&
            currentSecond >= 0 && currentSecond <= 59;
        const auto currentSeconds =
            (currentHour * 3'600) +
            (currentMinute * 60) +
            currentSecond;
        const auto targetSeconds =
            (static_cast<int>(state.hour) * 3'600) +
            (static_cast<int>(state.minute) * 60) +
            static_cast<int>(state.second);
        const auto linearDifference = currentClockValid
            ? std::abs(currentSeconds - targetSeconds)
            : 86'400;
        const auto circularDifference = currentClockValid
            ? std::min(
                  linearDifference,
                  86'400 - linearDifference)
            : 86'400;
        if (!currentClockValid || circularDifference > 2) {
            TIME::SET_CLOCK_TIME(
                state.hour,
                state.minute,
                state.second);
        }
        if (TIME::GET_CLOCK_DAY_OF_MONTH() !=
                static_cast<int>(state.day) ||
            TIME::GET_CLOCK_MONTH() !=
                static_cast<int>(state.month) ||
            TIME::GET_CLOCK_YEAR() !=
                static_cast<int>(state.year)) {
            TIME::SET_CLOCK_DATE(
                state.day,
                state.month,
                state.year);
        }
        TIME::PAUSE_CLOCK(TRUE, 0);

        if ((state.flags & kWeatherValid) != 0U &&
            state.weatherFrom != 0U &&
            state.weatherTo != 0U) {
            GAMEPLAY::_SET_WEATHER_TYPE_TRANSITION(
                static_cast<Hash>(state.weatherFrom),
                static_cast<Hash>(state.weatherTo),
                state.weatherBlend,
                TRUE);
            GAMEPLAY::FREEZE_WEATHER(TRUE);
        }
        if (!worldClockWeatherOverrideActive_) {
            Log(
                "host clock/date/weather authority applied on guest");
        }
        worldClockWeatherOverrideActive_ = true;
        return true;
#else
        (void)state;
#endif
    } catch (...) {
        // Host-world correction is best effort.
    }
    return false;
}

bool ScriptHookSdkFacade::ApplyRemoteEquipment(
    const EquipmentStatePayload& state) noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        constexpr auto kEquipped =
            static_cast<std::uint32_t>(
                EquipmentStateFlag::Equipped);
        constexpr auto kReloading =
            static_cast<std::uint32_t>(
                EquipmentStateFlag::Reloading);
        if (!state.entityId.IsValid() ||
            state.entityId != remotePlayerId_ ||
            (state.flags & ~(kEquipped | kReloading)) != 0U) {
            return false;
        }
        const auto handle =
            replicas_.FindLocal(state.entityId);
        if (!handle.has_value() ||
            ENTITY::DOES_ENTITY_EXIST(*handle) == FALSE) {
            return false;
        }

        char unarmedName[] = "WEAPON_UNARMED";
        const bool requestedEquipped =
            (state.flags & kEquipped) != 0U &&
            state.weaponHash != 0U;
        const auto requestedWeapon =
            requestedEquipped
                ? static_cast<Hash>(state.weaponHash)
                : GAMEPLAY::GET_HASH_KEY(unarmedName);
        if (requestedWeapon == 0U ||
            (requestedWeapon != static_cast<Hash>(kWeaponLasso) &&
             WEAPON::IS_WEAPON_VALID(requestedWeapon) == FALSE)) {
            return false;
        }

        const auto ammo = static_cast<int>(
            std::min<std::uint32_t>(
                state.ammo,
                static_cast<std::uint32_t>(
                    std::numeric_limits<int>::max())));
        const auto now = TickMilliseconds();
        if (remoteVisualFireSuppressedWeaponHash_ != 0U &&
            remoteVisualFireSuppressedWeaponHash_ !=
                static_cast<std::uint32_t>(requestedWeapon)) {
            RestoreRemoteVisualFireAmmo(*handle, now, true);
        } else {
            RestoreRemoteVisualFireAmmo(*handle, now, false);
        }
        remoteWeaponAmmo_ = static_cast<std::uint32_t>(ammo);
        if (remoteVisualFireSuppressedWeaponHash_ ==
            static_cast<std::uint32_t>(requestedWeapon)) {
            // EquipmentState is authoritative and can arrive while the short
            // dry-fire lease is active. Preserve its newer total for cleanup,
            // while keeping the local weapon empty until the task has ended.
            remoteVisualFireRestoreAmmo_ =
                static_cast<std::uint32_t>(ammo);
        }
        ++remoteActionEquipmentUpdates_;
        const bool requestedWeaponChanged =
            remoteWeaponHash_ !=
            static_cast<std::uint32_t>(requestedWeapon);
        if (requestedWeaponChanged) {
            remoteWeaponHash_ =
                static_cast<std::uint32_t>(requestedWeapon);
            previousRemoteWeaponVisualMs_ = 0U;
            remoteWeaponNextGrantMs_ = 0U;
            remoteWeaponGrantAttempts_ = 0U;
            remoteWeaponConfirmedOwned_ = false;
        }
        const bool weaponActionInterrupted =
            PED::IS_PED_RAGDOLL(*handle) != FALSE ||
            PED::IS_PED_FALLING(*handle) != FALSE ||
            AI::IS_PED_GETTING_UP(*handle) != FALSE;
        if (weaponActionInterrupted) {
            ++remoteActionEquipmentSuppressed_;
            return true;
        }
        auto ownsRequestedWeapon =
            !requestedEquipped ||
            WEAPON::HAS_PED_GOT_WEAPON(
                *handle,
                requestedWeapon,
                FALSE,
                FALSE) != FALSE;
        const bool grantCooldownElapsed =
            remoteWeaponNextGrantMs_ == 0U ||
            now >= remoteWeaponNextGrantMs_;
        if (!ownsRequestedWeapon &&
            !remoteWeaponConfirmedOwned_ &&
            remoteWeaponGrantAttempts_ < 2U &&
            grantCooldownElapsed) {
            WEAPON::GIVE_DELAYED_WEAPON_TO_PED(
                *handle,
                requestedWeapon,
                std::max(ammo, 1),
                TRUE,
                0);
            ++remoteActionWeaponGrants_;
            ++remoteWeaponGrantAttempts_;
            remoteWeaponNextGrantMs_ =
                now + kRemoteWeaponGrantRetryMilliseconds;
            ownsRequestedWeapon =
                WEAPON::HAS_PED_GOT_WEAPON(
                    *handle,
                    requestedWeapon,
                    FALSE,
                    FALSE) != FALSE;
        }
        if (ownsRequestedWeapon) {
            remoteWeaponNextGrantMs_ = 0U;
            remoteWeaponGrantAttempts_ = 0U;
            remoteWeaponConfirmedOwned_ = true;
            Hash currentWeapon{};
            const bool currentWeaponKnown =
                WEAPON::GET_CURRENT_PED_WEAPON(
                    *handle,
                    &currentWeapon,
                    FALSE,
                    0,
                    FALSE) != FALSE;
            if (!currentWeaponKnown ||
                currentWeapon != requestedWeapon) {
                WEAPON::SET_CURRENT_PED_WEAPON(
                    *handle,
                    requestedWeapon,
                    TRUE,
                    0,
                    FALSE,
                    FALSE);
            }
            const bool visualFireAmmoSuppressed =
                remoteVisualFireSuppressedWeaponHash_ ==
                    static_cast<std::uint32_t>(requestedWeapon) &&
                remoteVisualFireRestoreAtMs_ != 0U &&
                now < remoteVisualFireRestoreAtMs_;
            WEAPON::SET_PED_AMMO(
                *handle,
                requestedWeapon,
                visualFireAmmoSuppressed ? 0 : ammo);
            if (visualFireAmmoSuppressed) {
                (void)WEAPON::SET_AMMO_IN_CLIP(
                    *handle,
                    requestedWeapon,
                    0);
            }
        }
        const bool isReloading =
            (state.flags & kReloading) != 0U &&
            (state.flags & kEquipped) != 0U;
        const bool reloadStateChanged =
            isReloading != remoteReloading_;
        if (isReloading && !remoteReloading_) {
            AI::TASK_RELOAD_WEAPON(*handle, FALSE);
        }
        if (reloadStateChanged) {
            // TASK_RELOAD_WEAPON takes ownership from locomotion. Do not
            // mistake the old task bookkeeping for a live movement task:
            // while reloading the movement writer stays suppressed, and the
            // first transform after reload ends starts the current gait.
            previousRemoteTaskMs_ = 0U;
            previousRemoteTaskDestination_ = {};
            previousRemoteTaskHeading_ = 0.0F;
            hasRemoteLocomotionTask_ = false;
        }
        remoteReloading_ = isReloading;
        return true;
#else
        (void)state;
#endif
    } catch (...) {
        // Equipment replication is cosmetic on the receiving client.
    }
    return false;
}

bool ScriptHookSdkFacade::UnlockLocalWeaponEntitlement(
    const std::uint32_t weaponHash) noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        const auto weapon = static_cast<Hash>(weaponHash);
        if (weapon == 0U || WEAPON::IS_WEAPON_VALID(weapon) == FALSE) {
            return false;
        }
        const auto unlock = GetWeaponUnlock(weapon);
        if (unlock == 0U) {
            return false;
        }
        const auto visible = UNLOCK::_0x8588A14B75AF096B(unlock) != FALSE;
        const auto unlocked = UNLOCK::_0xC4B660C7B6040E75(unlock) != FALSE;
        if (visible && unlocked) {
            return true;
        }
        if (!visible) {
            UNLOCK::_0x46B901A8ECDB5A61(unlock, TRUE);
        }
        if (!unlocked) {
            UNLOCK::_0x1B7C5ADA8A6910A0(unlock, TRUE);
        }
        const bool applied =
            UNLOCK::_0x8588A14B75AF096B(unlock) != FALSE &&
            UNLOCK::_0xC4B660C7B6040E75(unlock) != FALSE;
        Log(std::string{"[WEAPON_ENTITLEMENT] weaponHash="} +
            std::to_string(weaponHash) + ", unlockHash=" +
            std::to_string(unlock) + ", visible=" +
            std::to_string(visible ? 1 : 0) + "->" +
            std::to_string(applied ? 1 : 0) + ", unlocked=" +
            std::to_string(unlocked ? 1 : 0) + "->" +
            std::to_string(applied ? 1 : 0));
        return applied;
#else
        (void)weaponHash;
#endif
    } catch (...) {
        // A local unlock hand-off must never interrupt normal replication.
    }
    return false;
}

bool ScriptHookSdkFacade::ApplyCampaignCapability(
    const CampaignCapabilityPayload& capability) noexcept {
    if (!IsSupportedCampaignCapability(capability)) {
        Log(std::string{"[CAPABILITY] unsupported record rejected kind="} +
            std::to_string(static_cast<unsigned int>(capability.kind)) +
            ", recordHash=" + std::to_string(capability.recordHash));
        return false;
    }
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
    try {
        if (capability.kind == CampaignCapabilityKind::WeaponShopEligibility) {
            return UnlockLocalWeaponEntitlement(capability.recordHash);
        }
        if (capability.kind == CampaignCapabilityKind::Recipe) {
            const auto unlock = static_cast<Hash>(capability.recordHash);
            UNLOCK::_0x46B901A8ECDB5A61(unlock, TRUE);
            UNLOCK::_0x1B7C5ADA8A6910A0(unlock, TRUE);
            return UNLOCK::_0x8588A14B75AF096B(unlock) != FALSE &&
                UNLOCK::_0xC4B660C7B6040E75(unlock) != FALSE;
        }
        Log("[CAPABILITY] unsupported capability kind rejected");
    } catch (...) {
        Log("[CAPABILITY] native application raised an exception");
    }
#else
    (void)capability;
#endif
    return false;
}

bool ScriptHookSdkFacade::MaintainRemoteMount(
    const PlayerMountStatePayload& state,
    const std::optional<PlayerMountStatePayload>& localState) noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        constexpr auto kPresent =
            static_cast<std::uint8_t>(
                PlayerMountStateFlag::Present);
        constexpr auto kMounted =
            static_cast<std::uint8_t>(
                PlayerMountStateFlag::Mounted);
        constexpr auto kDead =
            static_cast<std::uint8_t>(
                PlayerMountStateFlag::Dead);
        constexpr auto kBorrowedPeerMount =
            static_cast<std::uint8_t>(
                PlayerMountStateFlag::BorrowedPeerMount);
        constexpr auto kVehicle =
            static_cast<std::uint8_t>(PlayerMountStateFlag::Vehicle);
        constexpr auto kVehicleDriver =
            static_cast<std::uint8_t>(PlayerMountStateFlag::VehicleDriver);
        constexpr auto kVehiclePassenger =
            static_cast<std::uint8_t>(PlayerMountStateFlag::VehiclePassenger);
        if (!state.playerEntityId.IsValid() ||
            !state.mountEntityId.IsValid() ||
            state.playerEntityId != remotePlayerId_ ||
            state.playerEntityId == state.mountEntityId) {
            return false;
        }
        if ((state.flags & kPresent) == 0U) {
            ClearRemoteMount();
            return true;
        }
        const bool vehicle = (state.flags & kVehicle) != 0U;
        const bool vehicleDriver = (state.flags & kVehicleDriver) != 0U;
        const bool vehiclePassenger =
            (state.flags & kVehiclePassenger) != 0U;
        if (vehicle) {
            if (!vehicleDriver == !vehiclePassenger ||
                state.modelHash == 0U || !IsFinite(state.position) ||
                !IsFinite(state.velocity) || !std::isfinite(state.heading) ||
                state.heading < 0.0F || state.heading >= 360.0F) {
                return false;
            }
            const auto remotePlayer = replicas_.FindLocal(state.playerEntityId);
            if (!remotePlayer.has_value() ||
                *remotePlayer == PLAYER::PLAYER_PED_ID() ||
                ENTITY::DOES_ENTITY_EXIST(*remotePlayer) == FALSE) {
                return false;
            }
            const auto rider = static_cast<Ped>(*remotePlayer);
            const bool borrowed = (state.flags & kBorrowedPeerMount) != 0U;
            const bool aliasesLocalVehicle =
                localState.has_value() &&
                (localState->flags & kPresent) != 0U &&
                (localState->flags & kVehicle) != 0U &&
                (localState->flags & kBorrowedPeerMount) == 0U &&
                localState->mountEntityId == state.mountEntityId &&
                localState->generation == state.generation;
            Vehicle vehicleHandle{};
            if (borrowed || aliasesLocalVehicle) {
                if (!aliasesLocalVehicle || localKnownVehicleHandle_ == 0) {
                    return true;
                }
                vehicleHandle = static_cast<Vehicle>(localKnownVehicleHandle_);
                if (ENTITY::DOES_ENTITY_EXIST(vehicleHandle) == FALSE ||
                    static_cast<std::uint32_t>(
                        ENTITY::GET_ENTITY_MODEL(vehicleHandle)) != state.modelHash) {
                    return true;
                }
            } else {
                if (remoteVehicleId_.IsValid() &&
                    (remoteVehicleId_ != state.mountEntityId ||
                     remoteVehicleModelHash_ != state.modelHash ||
                     remoteVehicleGeneration_ != state.generation)) {
                    ClearRemoteMount();
                }
                remoteVehicleId_ = state.mountEntityId;
                remoteVehicleModelHash_ = state.modelHash;
                remoteVehicleGeneration_ = state.generation;
                auto handle = remoteVehicleReplicas_.FindLocal(state.mountEntityId);
                if (handle.has_value() &&
                    ENTITY::DOES_ENTITY_EXIST(*handle) == FALSE) {
                    (void)remoteVehicleReplicas_.Remove(state.mountEntityId);
                    handle.reset();
                }
                if (!handle.has_value()) {
                    const auto model = static_cast<Hash>(state.modelHash);
                    if (STREAMING::IS_MODEL_VALID(model) == FALSE ||
                        STREAMING::IS_MODEL_A_VEHICLE(model) == FALSE) {
                        return false;
                    }
                    if (remoteVehicleRequestedAtMs_ == 0U) {
                        remoteVehicleRequestedAtMs_ = TickMilliseconds();
                    }
                    STREAMING::REQUEST_MODEL(model, FALSE);
                    if (STREAMING::HAS_MODEL_LOADED(model) == FALSE) {
                        return true;
                    }
                    vehicleHandle = VEHICLE::CREATE_VEHICLE(
                        model, state.position.x, state.position.y, state.position.z,
                        state.heading, FALSE, FALSE, FALSE, FALSE);
                    STREAMING::SET_MODEL_AS_NO_LONGER_NEEDED(model);
                    if (vehicleHandle == 0) {
                        return true;
                    }
                    ENTITY::SET_ENTITY_AS_MISSION_ENTITY(vehicleHandle, TRUE, TRUE);
                    ENTITY::SET_ENTITY_CAN_BE_DAMAGED(vehicleHandle, FALSE);
                    ENTITY::SET_ENTITY_COLLISION(vehicleHandle, TRUE, TRUE);
                    if (!remoteVehicleReplicas_.Bind(
                            state.mountEntityId,
                            static_cast<LocalEntityHandle>(vehicleHandle))) {
                        VEHICLE::DELETE_VEHICLE(&vehicleHandle);
                        return false;
                    }
                    Log("[SHARED_WAGON] remote vehicle replica created");
                } else {
                    vehicleHandle = static_cast<Vehicle>(*handle);
                }
                ENTITY::SET_ENTITY_COORDS_NO_OFFSET(
                    vehicleHandle, state.position.x, state.position.y, state.position.z,
                    FALSE, FALSE, FALSE);
                ENTITY::SET_ENTITY_HEADING(vehicleHandle, state.heading);
                ENTITY::SET_ENTITY_VELOCITY(
                    vehicleHandle, state.velocity.x, state.velocity.y, state.velocity.z);
            }
            const auto desiredSeat = vehicleDriver ? -1 : 0;
            if (PED::GET_VEHICLE_PED_IS_IN(rider, FALSE) != vehicleHandle ||
                VEHICLE::GET_PED_IN_VEHICLE_SEAT(vehicleHandle, desiredSeat) != rider) {
                PED::SET_PED_INTO_VEHICLE(rider, vehicleHandle, desiredSeat);
            }
            return true;
        }
        const auto exactSceneRider = replicas_.FindLocal(state.playerEntityId);
        const auto exactSceneMount =
            remoteMountReplicas_.FindLocal(state.mountEntityId);
        if ((exactSceneRider.has_value() &&
             IsOwnedHybridAnimSceneEntity(*exactSceneRider)) ||
            (exactSceneMount.has_value() &&
             IsOwnedHybridAnimSceneEntity(*exactSceneMount))) {
            // Mount/dismount tasks and transform correction fight authored
            // mounted AnimScene clips. Once either role is bound, the native
            // scene owns both the relation and root motion.
            return true;
        }
        if (state.modelHash == 0U ||
            !IsFinite(state.position) ||
            !IsFinite(state.velocity) ||
            !std::isfinite(state.heading) ||
            state.heading < 0.0F ||
            state.heading >= 360.0F ||
            !std::isfinite(state.healthFraction) ||
            state.healthFraction < 0.0F ||
            state.healthFraction > 1.0F) {
            return false;
        }
        const auto now = TickMilliseconds();
        const auto reconcileRider = [&](
                                        const Ped mount,
                                        const bool shouldMount,
                                        const bool borrowed) {
            const auto remotePlayer =
                replicas_.FindLocal(state.playerEntityId);
            if (!remotePlayer.has_value() ||
                ENTITY::DOES_ENTITY_EXIST(*remotePlayer) == FALSE) {
                return;
            }
            const auto rider = static_cast<Ped>(*remotePlayer);
            const auto localPlayer = PLAYER::PLAYER_PED_ID();
            if (rider == 0 || rider == localPlayer ||
                replicas_.FindNetwork(
                    static_cast<LocalEntityHandle>(rider)) !=
                    std::optional{state.playerEntityId}) {
                // A remote mount relation must never mutate PLAYER_PED_ID.
                // Keep this fail-closed even if a stale/corrupt registry entry
                // survives a reconnect; mixing rider handles is otherwise
                // visible as the local player mounting/dismounting when the
                // peer touches a different horse.
                Log(
                    "[ERROR][REMOTE_MOUNT_RELATION] rejected a rider mapping that did not resolve exclusively to the remote replica");
                return;
            }
            auto actuallyMounted =
                PED::IS_PED_ON_MOUNT(rider) != FALSE;
            const bool onRequestedMount =
                actuallyMounted &&
                PED::GET_MOUNT(rider) == mount;
            if (shouldMount && !onRequestedMount) {
                const bool retryDue =
                    previousRemoteMountAttachAttemptMs_ == 0U ||
                    now < previousRemoteMountAttachAttemptMs_ ||
                    now - previousRemoteMountAttachAttemptMs_ >=
                        kRemoteMountRelationRetryMilliseconds;
                if (retryDue) {
                    SetPedOntoMount(rider, mount);
                    previousRemoteMountAttachAttemptMs_ = now;
                    ++remoteMountAttachAttempts_;
                }
            } else if (!shouldMount && actuallyMounted) {
                // Do not trust the bookkeeping bit here: the native task can
                // be interrupted by the player motor or another ScriptHook
                // task. Reissue until the engine confirms the rider is off.
                const bool retryDue =
                    previousRemoteMountDismountAttemptMs_ == 0U ||
                    now < previousRemoteMountDismountAttemptMs_ ||
                    now - previousRemoteMountDismountAttemptMs_ >=
                        kRemoteMountRelationRetryMilliseconds;
                if (retryDue) {
                    TaskDismountAnimal(rider);
                    previousRemoteMountDismountAttemptMs_ = now;
                    ++remoteMountDismountAttempts_;
                }
            }

            actuallyMounted =
                PED::IS_PED_ON_MOUNT(rider) != FALSE;
            remotePlayerMounted_ =
                actuallyMounted &&
                (!shouldMount ||
                 PED::GET_MOUNT(rider) == mount);
            if (shouldMount) {
                remotePlayerMountHandle_ =
                    static_cast<LocalEntityHandle>(mount);
                remotePlayerMountBorrowed_ = borrowed;
                previousRemoteMountDismountAttemptMs_ = 0U;
            } else if (!actuallyMounted) {
                remotePlayerMountHandle_ = 0;
                remotePlayerMountBorrowed_ = false;
                previousRemoteMountAttachAttemptMs_ = 0U;
                previousRemoteMountDismountAttemptMs_ = 0U;
            }
        };

        const bool wireBorrowed =
            (state.flags & kBorrowedPeerMount) != 0U;
        constexpr auto kLocalPresent =
            static_cast<std::uint8_t>(
                PlayerMountStateFlag::Present);
        const bool aliasesLocalMount =
            localState.has_value() &&
            (localState->flags & kLocalPresent) != 0U &&
            (localState->flags & kBorrowedPeerMount) == 0U &&
            localState->mountEntityId == state.mountEntityId &&
            localState->generation == state.generation;
        if (wireBorrowed || aliasesLocalMount) {
            // A peer riding our horse references the existing shared mount.
            // Never CREATE_PED or apply peer-authored transforms to it.
            if (!aliasesLocalMount ||
                localKnownMountHandle_ == 0) {
                return true;
            }
            const auto localMount =
                static_cast<Ped>(localKnownMountHandle_);
            if (ENTITY::DOES_ENTITY_EXIST(localMount) == FALSE ||
                static_cast<std::uint32_t>(
                    ENTITY::GET_ENTITY_MODEL(localMount)) !=
                    state.modelHash) {
                return true;
            }
            if (remoteMountReplicas_.Size() != 0U) {
                ClearRemoteMount();
            }
            const bool relationChanged =
                !remotePlayerMountBorrowed_ ||
                remotePlayerMountHandle_ !=
                    localKnownMountHandle_;
            reconcileRider(localMount, true, true);
            if (relationChanged) {
                Log(
                    "[REMOTE_MOUNT_RELATION] reused local shared horse; duplicate proxy suppressed");
            }
            return true;
        }

        if (remoteMountId_.IsValid() &&
            (remoteMountId_ != state.mountEntityId ||
             remoteMountModelHash_ != state.modelHash ||
             remoteMountGeneration_ != state.generation)) {
            ClearRemoteMount();
        }
        remoteMountId_ = state.mountEntityId;
        remoteMountModelHash_ = state.modelHash;
        remoteMountGeneration_ = state.generation;

        auto handle =
            remoteMountReplicas_.FindLocal(
                state.mountEntityId);
        if (handle.has_value() &&
            ENTITY::DOES_ENTITY_EXIST(*handle) == FALSE) {
            (void)remoteMountReplicas_.Remove(
                state.mountEntityId);
            handle.reset();
            previousRemoteMountTaskMs_ = 0U;
            previousRemoteMountTransformMs_ = 0U;
            previousRemoteMountTaskDestination_ = {};
            remoteMountMoving_ = false;
        }
        const auto model =
            static_cast<Hash>(state.modelHash);
        if (!handle.has_value()) {
            if (STREAMING::IS_MODEL_VALID(model) == FALSE) {
                return false;
            }
            if (remoteMountRequestedAtMs_ == 0U) {
                remoteMountRequestedAtMs_ = now;
            }
            STREAMING::REQUEST_MODEL(model, FALSE);
            if (STREAMING::HAS_MODEL_LOADED(model) == FALSE) {
                return true;
            }
            auto mount = PED::CREATE_PED(
                model,
                state.position.x,
                state.position.y,
                state.position.z,
                state.heading,
                FALSE,
                FALSE,
                FALSE,
                FALSE);
            STREAMING::SET_MODEL_AS_NO_LONGER_NEEDED(model);
            if (mount == 0) {
                return now < remoteMountRequestedAtMs_ ||
                       now - remoteMountRequestedAtMs_ <
                           kWorldModelLoadTimeoutMilliseconds;
            }
            SetRandomOutfitVariation(mount);
            ENTITY::SET_ENTITY_AS_MISSION_ENTITY(
                mount,
                TRUE,
                TRUE);
            ENTITY::SET_ENTITY_LOAD_COLLISION_FLAG(
                mount,
                TRUE);
            ENTITY::SET_ENTITY_HAS_GRAVITY(mount, TRUE);
            ENTITY::SET_ENTITY_COLLISION(mount, TRUE, TRUE);
            ENTITY::SET_ENTITY_CAN_BE_DAMAGED(mount, FALSE);
            ENTITY::SET_ENTITY_VISIBLE(mount, TRUE);
            ENTITY::RESET_ENTITY_ALPHA(mount);
            PED::SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(
                mount,
                TRUE);
            PED::SET_PED_KEEP_TASK(mount, TRUE);
            if (!remoteMountReplicas_.Bind(
                    state.mountEntityId,
                    static_cast<LocalEntityHandle>(
                        mount))) {
                PED::DELETE_PED(&mount);
                return false;
            }
            handle =
                static_cast<LocalEntityHandle>(mount);
            remoteMountRequestedAtMs_ = 0U;
            Log(
                "player mount v6 proxy created for remote slot " +
                std::to_string(
                    static_cast<std::uint8_t>(
                        state.slot)));
        }

        const auto mount = static_cast<Ped>(*handle);
        // Guest population suppression used to hide this mission-owned horse
        // after it was spawned. Reassert the cosmetic proxy invariants so a
        // script/population transition cannot leave an interactive invisible
        // mount behind.
        ENTITY::SET_ENTITY_VISIBLE(mount, TRUE);
        ENTITY::RESET_ENTITY_ALPHA(mount);
        ENTITY::SET_ENTITY_COLLISION(mount, TRUE, TRUE);
        const auto current = ToBridgeVector(
            ENTITY::GET_ENTITY_COORDS(
                mount,
                TRUE,
                FALSE));
        const auto error =
            Distance(current, state.position);
        const auto horizontalError = std::hypot(
            state.position.x - current.x,
            state.position.y - current.y);
        const auto mountApplyGap =
            previousRemoteMountTransformMs_ == 0U ||
                    now <= previousRemoteMountTransformMs_
                ? 0U
                : now - previousRemoteMountTransformMs_;
        ++remoteMountSamples_;
        remoteMountMaximumApplyGapMs_ = std::max(
            remoteMountMaximumApplyGapMs_,
            mountApplyGap);
        if (std::isfinite(error)) {
            remoteMountMaximumError_ = std::max(
                remoteMountMaximumError_,
                error);
        }
        const auto horizontalSpeed = std::hypot(
            state.velocity.x,
            state.velocity.y);
        const bool moving =
            std::isfinite(horizontalSpeed) &&
            horizontalSpeed > 0.20F;
        const auto correctionThreshold =
            moving
                ? kRemoteMountMovingCorrectionMeters
                : kRemoteMountIdleCorrectionMeters;
        if (std::isfinite(error) &&
            std::isfinite(horizontalError) &&
            (horizontalError >= correctionThreshold ||
             error >= kRemoteMountHardCorrectionMeters)) {
            Vec3 corrected = state.position;
            if (error < kRemoteMountHardCorrectionMeters) {
                const auto elapsedSeconds =
                    previousRemoteMountTransformMs_ == 0U ||
                            now <= previousRemoteMountTransformMs_
                        ? 0.05F
                        : std::clamp(
                              static_cast<float>(
                                  now - previousRemoteMountTransformMs_) /
                                  1'000.0F,
                              0.001F,
                              0.10F);
                const auto alpha = 1.0F - std::exp(
                    -(moving ? 4.5F : 9.0F) * elapsedSeconds);
                corrected = {
                    current.x +
                        (state.position.x - current.x) *
                            alpha,
                    current.y +
                        (state.position.y - current.y) *
                            alpha,
                    // Let the local horse/navmesh own ground height and hoof
                    // IK. Per-frame Z warps caused the visible slope hops.
                    current.z};
            } else {
                ++remoteMountHardCorrections_;
            }
            ENTITY::SET_ENTITY_COORDS_NO_OFFSET(
                mount,
                corrected.x,
                corrected.y,
                corrected.z,
                TRUE,
                TRUE,
                FALSE);
            ++remoteMountCorrections_;
        }
        previousRemoteMountTransformMs_ = now;
        const Vec3 taskDestination{
            state.position.x + state.velocity.x * 0.70F,
            state.position.y + state.velocity.y * 0.70F,
            state.position.z + state.velocity.z * 0.70F};
        const auto movementHeading =
            moving
                ? NormalizeHeading(
                      static_cast<float>(
                          std::atan2(
                              -state.velocity.x,
                              state.velocity.y) *
                          (180.0 / std::numbers::pi)))
                : state.heading;
        const bool firstOrClockReset =
            previousRemoteMountTaskMs_ == 0U ||
            now < previousRemoteMountTaskMs_;
        const auto taskAge =
            firstOrClockReset
                ? 0U
                : now - previousRemoteMountTaskMs_;
        const bool taskChangeRequested =
            (moving &&
             taskAge >= kRemoteMountTaskRefreshMilliseconds) ||
            Distance(
                previousRemoteMountTaskDestination_,
                taskDestination) >=
                kRemoteMountTaskDestinationRefreshMeters ||
            remoteMountMoving_ != moving;
        const bool mountTaskRefresh =
            firstOrClockReset ||
            (taskAge >= kRemoteMountTaskMinimumRefreshMilliseconds &&
             taskChangeRequested);
        if (mountTaskRefresh) {
            if (moving) {
                AI::TASK_GO_STRAIGHT_TO_COORD(
                    mount,
                    taskDestination.x,
                    taskDestination.y,
                    taskDestination.z,
                    std::clamp(horizontalSpeed, 1.0F, 12.0F),
                    3'000,
                    movementHeading,
                    0.2F,
                    0);
            } else {
                AI::TASK_STAND_STILL(mount, 1'250);
                ENTITY::SET_ENTITY_HEADING(
                    mount,
                    state.heading);
            }
            PED::SET_PED_KEEP_TASK(mount, TRUE);
            previousRemoteMountTaskMs_ = now;
            previousRemoteMountTaskDestination_ =
                taskDestination;
            remoteMountMoving_ = moving;
            ++remoteMountTaskStarts_;
        }
        AI::SET_PED_DESIRED_MOVE_BLEND_RATIO(
            mount,
            moving
                ? std::clamp(horizontalSpeed / 3.5F, 1.0F, 3.0F)
                : 0.0F);
        PED::SET_PED_MAX_MOVE_BLEND_RATIO(mount, 3.0F);
        PED::SET_PED_MOVE_RATE_OVERRIDE(mount, 1.0F);

        if (remoteMountDiagnosticsStartedMs_ == 0U) {
            remoteMountDiagnosticsStartedMs_ = now;
        } else if (
            now >= remoteMountDiagnosticsStartedMs_ &&
            now - remoteMountDiagnosticsStartedMs_ >=
                kRemoteMotionDiagnosticsMilliseconds) {
            Log(
                "[REMOTE_MOUNT] samples=" +
                std::to_string(remoteMountSamples_) +
                ", max-error-m=" +
                std::to_string(remoteMountMaximumError_) +
                ", corrections=" +
                std::to_string(remoteMountCorrections_) +
                ", hard-corrections=" +
                std::to_string(remoteMountHardCorrections_) +
                ", task-starts=" +
                std::to_string(remoteMountTaskStarts_) +
                ", attach-attempts=" +
                std::to_string(remoteMountAttachAttempts_) +
                ", dismount-attempts=" +
                std::to_string(remoteMountDismountAttempts_) +
                ", max-apply-gap-ms=" +
                std::to_string(remoteMountMaximumApplyGapMs_) +
                ", moving=" +
                (moving ? std::string{"true"} : std::string{"false"}) +
                ", rider-mounted=" +
                (remotePlayerMounted_
                     ? std::string{"true"}
                     : std::string{"false"}) +
                ", shared-borrowed=" +
                (remotePlayerMountBorrowed_
                     ? std::string{"true"}
                     : std::string{"false"}));
            remoteMountDiagnosticsStartedMs_ = now;
            remoteMountSamples_ = 0U;
            remoteMountCorrections_ = 0U;
            remoteMountHardCorrections_ = 0U;
            remoteMountTaskStarts_ = 0U;
            remoteMountAttachAttempts_ = 0U;
            remoteMountDismountAttempts_ = 0U;
            remoteMountMaximumApplyGapMs_ = 0U;
            remoteMountMaximumError_ = 0.0F;
        }

        const auto maximumHealth =
            std::max(
                ENTITY::GET_ENTITY_MAX_HEALTH(
                    mount,
                    FALSE),
                1);
        const auto targetHealth =
            (state.flags & kDead) != 0U
                ? 0
                : static_cast<int>(
                      std::lround(
                          state.healthFraction *
                          static_cast<float>(
                              maximumHealth)));
        if (std::abs(
                ENTITY::GET_ENTITY_HEALTH(mount) -
                targetHealth) > 1) {
            ENTITY::SET_ENTITY_HEALTH(
                mount,
                targetHealth,
                0);
        }

        const bool shouldMount =
            (state.flags & kMounted) != 0U;
        reconcileRider(mount, shouldMount, false);
        return true;
#else
        (void)state;
        (void)localState;
#endif
    } catch (...) {
        // A mount is cosmetic replication; never let a bad model or native
        // destabilize the bridge.
    }
    return false;
}

void ScriptHookSdkFacade::ClearRemoteMount() noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        const auto remotePlayer =
            replicas_.FindLocal(remotePlayerId_);
        if (remotePlayer.has_value() &&
            *remotePlayer != PLAYER::PLAYER_PED_ID() &&
            replicas_.FindNetwork(*remotePlayer) ==
                std::optional{remotePlayerId_} &&
            ENTITY::DOES_ENTITY_EXIST(*remotePlayer) != FALSE &&
            PED::IS_PED_ON_MOUNT(
                static_cast<Ped>(*remotePlayer)) != FALSE) {
            TaskDismountAnimal(
                static_cast<Ped>(*remotePlayer));
        }
        for (const auto handle :
             remoteMountReplicas_.Drain()) {
            auto mount = static_cast<Ped>(handle);
            if (mount != 0 &&
                ENTITY::DOES_ENTITY_EXIST(mount) != FALSE) {
                PED::DELETE_PED(&mount);
            }
        }
        for (const auto handle : remoteVehicleReplicas_.Drain()) {
            auto vehicle = static_cast<Vehicle>(handle);
            if (vehicle != 0 &&
                ENTITY::DOES_ENTITY_EXIST(vehicle) != FALSE) {
                VEHICLE::DELETE_VEHICLE(&vehicle);
            }
        }
        if (remoteMountModelHash_ != 0U) {
            STREAMING::SET_MODEL_AS_NO_LONGER_NEEDED(
                static_cast<Hash>(
                    remoteMountModelHash_));
        }
#endif
    } catch (...) {
        remoteMountReplicas_.Clear();
    }
    remoteMountId_ = NetEntityId{};
    remoteMountModelHash_ = 0U;
    remoteMountGeneration_ = 0U;
    remoteMountRequestedAtMs_ = 0U;
    remoteVehicleId_ = NetEntityId{};
    remoteVehicleModelHash_ = 0U;
    remoteVehicleGeneration_ = 0U;
    remoteVehicleRequestedAtMs_ = 0U;
    previousRemoteMountTaskMs_ = 0U;
    previousRemoteMountTransformMs_ = 0U;
    remoteMountDiagnosticsStartedMs_ = 0U;
    remoteMountSamples_ = 0U;
    remoteMountCorrections_ = 0U;
    remoteMountHardCorrections_ = 0U;
    remoteMountTaskStarts_ = 0U;
    remoteMountAttachAttempts_ = 0U;
    remoteMountDismountAttempts_ = 0U;
    remoteMountMaximumApplyGapMs_ = 0U;
    previousRemoteMountAttachAttemptMs_ = 0U;
    previousRemoteMountDismountAttemptMs_ = 0U;
    remoteMountMaximumError_ = 0.0F;
    previousRemoteMountTaskDestination_ = {};
    remotePlayerMountHandle_ = 0;
    remoteMountMoving_ = false;
    remotePlayerMounted_ = false;
    remotePlayerMountBorrowed_ = false;
}

bool ScriptHookSdkFacade::SpawnWorldEntityProxy(
    const WorldEntityStatePayload& state) noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        if (!state.entityId.IsValid() ||
            state.entityId == remotePlayerId_ ||
            state.modelHash == 0U ||
            (state.kind != WorldEntityKind::Ped &&
             state.kind != WorldEntityKind::Object) ||
            !IsFinite(state.position) ||
            !IsFinite(state.velocity) ||
            !IsFinite(state.taskTarget) ||
            !std::isfinite(state.heading) ||
            state.heading < 0.0F ||
            state.heading >= 360.0F ||
            !std::isfinite(state.healthFraction) ||
            state.healthFraction < 0.0F ||
            state.healthFraction > 1.0F) {
            return false;
        }

        const auto existing = worldProxyEntries_.find(
            state.entityId);
        if (existing != worldProxyEntries_.end() &&
            (existing->second.state.modelHash != state.modelHash ||
             existing->second.state.kind != state.kind)) {
            DespawnWorldEntityProxy(state.entityId);
        }

        const auto now = TickMilliseconds();
        auto [iterator, inserted] =
            worldProxyEntries_.try_emplace(
                state.entityId,
                WorldProxyEntry{
                    state,
                    now,
                    now});
        if (!inserted) {
            iterator->second.state = state;
            iterator->second.receivedAtMs = now;
        }
        STREAMING::REQUEST_MODEL(
            static_cast<Hash>(state.modelHash),
            FALSE);
        return true;
#else
        (void)state;
#endif
    } catch (...) {
        // Pending proxy creation is fail-closed on malformed state/OOM.
    }
    return false;
}

bool ScriptHookSdkFacade::UpdateWorldEntityProxy(
    const WorldEntityStatePayload& state) noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        const auto iterator =
            worldProxyEntries_.find(state.entityId);
        if (iterator == worldProxyEntries_.end()) {
            // EntityUpdate is an upsert. This makes the unreliable snapshot
            // lane self-healing if a reliable spawn was lost during a pipe
            // reconnect.
            return SpawnWorldEntityProxy(state);
        }
        if (iterator->second.state.modelHash != state.modelHash ||
            iterator->second.state.kind != state.kind) {
            DespawnWorldEntityProxy(state.entityId);
            return SpawnWorldEntityProxy(state);
        }
        if (!IsFinite(state.position) ||
            !IsFinite(state.velocity) ||
            !IsFinite(state.taskTarget) ||
            !std::isfinite(state.heading) ||
            state.heading < 0.0F ||
            state.heading >= 360.0F ||
            !std::isfinite(state.healthFraction) ||
            state.healthFraction < 0.0F ||
            state.healthFraction > 1.0F) {
            return false;
        }
        iterator->second.state = state;
        iterator->second.receivedAtMs = TickMilliseconds();
        return true;
#else
        (void)state;
#endif
    } catch (...) {
    }
    return false;
}

void ScriptHookSdkFacade::DespawnWorldEntityProxy(
    const NetEntityId entityId) noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        WorldEntityKind kind{WorldEntityKind::Ped};
        const auto entry = worldProxyEntries_.find(entityId);
        if (entry != worldProxyEntries_.end()) {
            kind = entry->second.state.kind;
            STREAMING::SET_MODEL_AS_NO_LONGER_NEEDED(
                static_cast<Hash>(
                    entry->second.state.modelHash));
            worldProxyEntries_.erase(entry);
        }
        const auto handle =
            worldEntityReplicas_.FindLocal(entityId);
        if (!handle.has_value()) {
            return;
        }
        auto entity = static_cast<Entity>(*handle);
        (void)worldEntityReplicas_.Remove(entityId);
        if (ENTITY::DOES_ENTITY_EXIST(entity) != FALSE) {
            if (kind == WorldEntityKind::Object) {
                auto object = static_cast<Object>(entity);
                OBJECT::DELETE_OBJECT(&object);
            } else {
                auto ped = static_cast<Ped>(entity);
                PED::DELETE_PED(&ped);
            }
        }
#else
        (void)entityId;
#endif
    } catch (...) {
        // Cleanup is best effort and must remain safe during ASI unload.
    }
}

void ScriptHookSdkFacade::RestoreHiddenAmbientPeds() noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        for (const auto& [handle, entry] :
             hiddenAmbientPeds_) {
            const auto ped = static_cast<Ped>(handle);
            if (ped == 0 ||
                ENTITY::DOES_ENTITY_EXIST(ped) == FALSE ||
                static_cast<std::uint32_t>(
                    ENTITY::GET_ENTITY_MODEL(ped)) !=
                    entry.modelHash) {
                continue;
            }
            ENTITY::SET_ENTITY_VISIBLE(
                ped,
                entry.wasVisible ? TRUE : FALSE);
            if (entry.wasVisible) {
                ENTITY::RESET_ENTITY_ALPHA(ped);
            }
            ENTITY::SET_ENTITY_COLLISION(ped, TRUE, TRUE);
        }
        for (const auto& [handle, entry] :
             hiddenAmbientAttachments_) {
            const auto entity = static_cast<Entity>(handle);
            if (entity == 0 ||
                ENTITY::DOES_ENTITY_EXIST(entity) == FALSE ||
                static_cast<std::uint32_t>(
                    ENTITY::GET_ENTITY_MODEL(entity)) !=
                    entry.modelHash) {
                continue;
            }
            ENTITY::SET_ENTITY_VISIBLE(
                entity,
                entry.wasVisible ? TRUE : FALSE);
            if (entry.wasVisible) {
                ENTITY::RESET_ENTITY_ALPHA(entity);
            }
            ENTITY::SET_ENTITY_COLLISION(entity, TRUE, TRUE);
        }
        hiddenAmbientPeds_.clear();
        hiddenAmbientAttachments_.clear();
#endif
    } catch (...) {
        hiddenAmbientPeds_.clear();
        hiddenAmbientAttachments_.clear();
    }
}

void ScriptHookSdkFacade::MaintainHiddenPedAttachments() noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        std::array<int, kWorldPedPoolCapacity> objects{};
        const auto count = std::clamp(
            worldGetAllObjects(
                objects.data(),
                static_cast<int>(objects.size())),
            0,
            static_cast<int>(objects.size()));
        std::unordered_set<LocalEntityHandle> shouldHide;
        // Held weapon entities are not guaranteed to appear in the generic
        // object pool during an AnimScene transition. Query them directly so
        // a hidden guest-local actor cannot leave a floating rifle/revolver in
        // the replicated host camera.
        for (const auto& [handle, entry] : hiddenAmbientPeds_) {
            const auto ped = static_cast<Ped>(handle);
            if (ped == 0 ||
                ENTITY::DOES_ENTITY_EXIST(ped) == FALSE ||
                static_cast<std::uint32_t>(
                    ENTITY::GET_ENTITY_MODEL(ped)) != entry.modelHash) {
                continue;
            }
            const auto weaponEntity =
                WEAPON::GET_CURRENT_PED_WEAPON_ENTITY_INDEX(ped, 0);
            if (weaponEntity != 0 &&
                ENTITY::DOES_ENTITY_EXIST(weaponEntity) != FALSE) {
                shouldHide.insert(
                    static_cast<LocalEntityHandle>(weaponEntity));
            }
        }
        for (int index = 0; index < count; ++index) {
            const auto entity = static_cast<Entity>(
                objects[static_cast<std::size_t>(index)]);
            if (entity == 0 ||
                ENTITY::DOES_ENTITY_EXIST(entity) == FALSE ||
                ENTITY::IS_ENTITY_ATTACHED(entity) == FALSE) {
                continue;
            }
            auto parent = ENTITY::GET_ENTITY_ATTACHED_TO(entity);
            bool attachedToHiddenPed{};
            for (int depth = 0;
                 depth < 4 && parent != 0;
                 ++depth) {
                if (hiddenAmbientPeds_.contains(
                        static_cast<LocalEntityHandle>(parent))) {
                    attachedToHiddenPed = true;
                    break;
                }
                if (ENTITY::DOES_ENTITY_EXIST(parent) == FALSE ||
                    ENTITY::IS_ENTITY_ATTACHED(parent) == FALSE) {
                    break;
                }
                parent = ENTITY::GET_ENTITY_ATTACHED_TO(parent);
            }
            if (!attachedToHiddenPed) {
                continue;
            }
            shouldHide.insert(
                static_cast<LocalEntityHandle>(entity));
        }
        for (auto iterator = hiddenAmbientAttachments_.begin();
             iterator != hiddenAmbientAttachments_.end();) {
            const auto entity = static_cast<Entity>(iterator->first);
            const bool sameEntity =
                entity != 0 &&
                ENTITY::DOES_ENTITY_EXIST(entity) != FALSE &&
                static_cast<std::uint32_t>(
                    ENTITY::GET_ENTITY_MODEL(entity)) ==
                    iterator->second.modelHash;
            if (sameEntity && shouldHide.contains(iterator->first)) {
                ENTITY::SET_ENTITY_VISIBLE(entity, FALSE);
                ENTITY::SET_ENTITY_ALPHA(entity, 0, FALSE);
                ENTITY::SET_ENTITY_COLLISION(entity, FALSE, TRUE);
                ++iterator;
                continue;
            }
            if (sameEntity) {
                ENTITY::SET_ENTITY_VISIBLE(
                    entity,
                    iterator->second.wasVisible ? TRUE : FALSE);
                if (iterator->second.wasVisible) {
                    ENTITY::RESET_ENTITY_ALPHA(entity);
                }
                ENTITY::SET_ENTITY_COLLISION(entity, TRUE, TRUE);
            }
            iterator = hiddenAmbientAttachments_.erase(iterator);
        }
        for (const auto handle : shouldHide) {
            if (hiddenAmbientAttachments_.contains(handle)) {
                continue;
            }
            const auto entity = static_cast<Entity>(handle);
            hiddenAmbientAttachments_.emplace(
                handle,
                HiddenAmbientEntry{
                    static_cast<std::uint32_t>(
                        ENTITY::GET_ENTITY_MODEL(entity)),
                    ENTITY::IS_ENTITY_VISIBLE(entity) != FALSE ||
                        ENTITY::GET_ENTITY_ALPHA(entity) > 0});
            ENTITY::SET_ENTITY_VISIBLE(entity, FALSE);
            ENTITY::SET_ENTITY_ALPHA(entity, 0, FALSE);
            ENTITY::SET_ENTITY_COLLISION(entity, FALSE, TRUE);
        }
#endif
    } catch (...) {
        // The reversible ped mask remains useful even when an attachment
        // disappears while the world pool is being enumerated.
    }
}

void ScriptHookSdkFacade::CleanupWorldEntityProxies() noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        std::unordered_map<LocalEntityHandle, WorldEntityKind>
            kindsByHandle;
        for (const auto& [entityId, entry] : worldProxyEntries_) {
            const auto handle = worldEntityReplicas_.FindLocal(entityId);
            if (handle.has_value()) {
                kindsByHandle.emplace(*handle, entry.state.kind);
            }
        }
        for (const auto handle : worldEntityReplicas_.Drain()) {
            auto entity = static_cast<Entity>(handle);
            if (entity != 0 &&
                ENTITY::DOES_ENTITY_EXIST(entity) != FALSE) {
                const auto kind = kindsByHandle.find(handle);
                if (kind != kindsByHandle.end() &&
                    kind->second == WorldEntityKind::Object) {
                    auto object = static_cast<Object>(entity);
                    OBJECT::DELETE_OBJECT(&object);
                } else {
                    auto ped = static_cast<Ped>(entity);
                    PED::DELETE_PED(&ped);
                }
            }
        }
        for (const auto& [entityId, entry] :
             worldProxyEntries_) {
            (void)entityId;
            STREAMING::SET_MODEL_AS_NO_LONGER_NEEDED(
                static_cast<Hash>(
                    entry.state.modelHash));
        }
        worldProxyEntries_.clear();
#endif
    } catch (...) {
        worldEntityReplicas_.Clear();
        worldProxyEntries_.clear();
    }
}

bool ScriptHookSdkFacade::IsOwnedHybridAnimSceneEntity(
    const LocalEntityHandle handle) const noexcept {
    return ownedHybridAnimSceneHandle_ > 0 && handle != 0 &&
           std::find(
               ownedHybridAnimSceneBoundEntities_.begin(),
               ownedHybridAnimSceneBoundEntities_.end(),
               handle) != ownedHybridAnimSceneBoundEntities_.end();
}

void ScriptHookSdkFacade::MaintainWorldMirrorGuest(
    const bool active,
    const bool authoritativePopulationReady,
    const float radiusMeters) noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        if (!active ||
            !std::isfinite(radiusMeters) ||
            radiusMeters <= 0.0F) {
            CleanupWorldEntityProxies();
            RestoreHiddenAmbientPeds();
            worldMirrorGuestActive_ = false;
            previousWorldMirrorMaintainMs_ = 0U;
            previousWorldMirrorDiagnosticsMs_ = 0U;
            previousWorldDamageIntentMs_ = 0U;
            return;
        }

        const auto now = TickMilliseconds();
        const auto elapsedSeconds =
            previousWorldMirrorMaintainMs_ == 0U ||
                    now < previousWorldMirrorMaintainMs_
                ? 0.016F
                : std::clamp(
                      static_cast<float>(
                          now -
                          previousWorldMirrorMaintainMs_) /
                          1'000.0F,
                      0.001F,
                      0.1F);
        previousWorldMirrorMaintainMs_ = now;
        worldMirrorGuestActive_ = true;

        std::vector<NetEntityId> maintenanceOrder;
        maintenanceOrder.reserve(worldProxyEntries_.size());
        for (const auto& [entityId, entry] : worldProxyEntries_) {
            (void)entry;
            maintenanceOrder.push_back(entityId);
        }
        const auto dependencyDepth = [&](const NetEntityId entityId) {
            std::size_t depth{};
            auto iterator = worldProxyEntries_.find(entityId);
            std::unordered_set<NetEntityId, NetEntityIdHash> visited;
            while (iterator != worldProxyEntries_.end() &&
                   iterator->second.state.parentEntityId.IsValid() &&
                   visited.insert(iterator->first).second &&
                   depth <= worldProxyEntries_.size()) {
                ++depth;
                iterator = worldProxyEntries_.find(
                    iterator->second.state.parentEntityId);
            }
            return depth;
        };
        std::ranges::sort(
            maintenanceOrder,
            [&](const auto lhs, const auto rhs) {
                const auto lhsDepth = dependencyDepth(lhs);
                const auto rhsDepth = dependencyDepth(rhs);
                if (lhsDepth != rhsDepth) {
                    return lhsDepth < rhsDepth;
                }
                return lhs < rhs;
            });

        for (const auto entityId : maintenanceOrder) {
            auto& entry = worldProxyEntries_.at(entityId);
            auto handle =
                worldEntityReplicas_.FindLocal(entityId);
            if (handle.has_value() &&
                ENTITY::DOES_ENTITY_EXIST(*handle) == FALSE) {
                (void)worldEntityReplicas_.Remove(entityId);
                handle.reset();
                entry.requestedAtMs = now;
                entry.weaponHash = 0U;
                entry.aiming = false;
                entry.spawnDisposition =
                    WorldProxySpawnDisposition::RetryableFailure;
                entry.nextSpawnRetryMs = now + 250U;
            }

            const auto model =
                static_cast<Hash>(
                    entry.state.modelHash);
            if (!handle.has_value()) {
                if (entry.spawnDisposition ==
                    WorldProxySpawnDisposition::PermanentFailure) {
                    continue;
                }
                const bool requiresParent =
                    (entry.state.flags &
                     static_cast<std::uint8_t>(
                         WorldEntityStateFlag::Mounted)) != 0U;
                if (requiresParent) {
                    const auto parent =
                        worldEntityReplicas_.FindLocal(
                            entry.state.parentEntityId);
                    if (!parent.has_value() ||
                        ENTITY::DOES_ENTITY_EXIST(*parent) == FALSE) {
                        // Model loading and pool order are independent. Keep
                        // a rider pending until the parent mount is a real
                        // local entity, not merely a node in desired state.
                        entry.spawnDisposition =
                            WorldProxySpawnDisposition::PendingDependency;
                        continue;
                    }
                }
                if (STREAMING::IS_MODEL_VALID(model) == FALSE) {
                    entry.spawnDisposition =
                        WorldProxySpawnDisposition::PermanentFailure;
                    if (!entry.permanentFailureLogged) {
                        entry.permanentFailureLogged = true;
                        Log(
                            "[ERROR][ENTITY_SPAWN] permanent invalid model for entity " +
                            std::to_string(entityId.Value()) +
                            "; desired node retained without spawn flicker");
                    }
                    continue;
                }
                STREAMING::REQUEST_MODEL(model, FALSE);
                if (STREAMING::HAS_MODEL_LOADED(model) == FALSE) {
                    entry.spawnDisposition =
                        WorldProxySpawnDisposition::PendingModel;
                    if (now >= entry.requestedAtMs &&
                        now - entry.requestedAtMs >=
                            kWorldModelLoadTimeoutMilliseconds &&
                        !entry.modelWaitLogged) {
                        entry.modelWaitLogged = true;
                        Log(
                            "[ENTITY_SPAWN_PENDING_MODEL] model stream exceeded 5s for entity " +
                            std::to_string(entityId.Value()) +
                            "; request remains pending and is not counted as spawn failure");
                    }
                    continue;
                }

                if (entry.nextSpawnRetryMs != 0U &&
                    now < entry.nextSpawnRetryMs) {
                    continue;
                }

                const bool objectKind =
                    entry.state.kind == WorldEntityKind::Object;
                auto entity = objectKind
                                  ? static_cast<Entity>(
                                        OBJECT::CREATE_OBJECT(
                                            model,
                                            entry.state.position.x,
                                            entry.state.position.y,
                                            entry.state.position.z,
                                            FALSE,
                                            FALSE,
                                            TRUE,
                                            0,
                                            0))
                                  : static_cast<Entity>(
                                        PED::CREATE_PED(
                                            model,
                                            entry.state.position.x,
                                            entry.state.position.y,
                                            entry.state.position.z,
                                            entry.state.heading,
                                            FALSE,
                                            FALSE,
                                            FALSE,
                                            FALSE));
                STREAMING::SET_MODEL_AS_NO_LONGER_NEEDED(
                    model);
                if (entity == 0) {
                    entry.spawnDisposition =
                        WorldProxySpawnDisposition::RetryableFailure;
                    entry.spawnAttempts = std::min(
                        entry.spawnAttempts + 1U,
                        32U);
                    const auto retryDelay = std::min<std::uint64_t>(
                        2'000U,
                        250U * static_cast<std::uint64_t>(
                            std::max(entry.spawnAttempts, 1U)));
                    entry.nextSpawnRetryMs = now + retryDelay;
                    if (entry.spawnAttempts <= 2U ||
                        (entry.spawnAttempts &
                         (entry.spawnAttempts - 1U)) == 0U) {
                        Log(
                            "[WARNING][ENTITY_SPAWN_RETRYABLE] native entity creation returned zero for entity " +
                            std::to_string(entityId.Value()) +
                            "; attempt=" +
                            std::to_string(entry.spawnAttempts) +
                            ", retry-ms=" +
                            std::to_string(retryDelay));
                    }
                    continue;
                }

                const auto ped = static_cast<Ped>(entity);
                // CREATE_PED may produce a valid, collidable MetaPed with no
                // visible components until its outfit is initialized.
                if (!objectKind) {
                    SetRandomOutfitVariation(ped);
                }
                ENTITY::SET_ENTITY_AS_MISSION_ENTITY(
                    entity,
                    TRUE,
                    TRUE);
                ENTITY::SET_ENTITY_LOAD_COLLISION_FLAG(
                    entity,
                    TRUE);
                ENTITY::SET_ENTITY_HAS_GRAVITY(entity, objectKind ? FALSE : TRUE);
                ENTITY::SET_ENTITY_COLLISION(entity, objectKind ? FALSE : TRUE, TRUE);
                ENTITY::SET_ENTITY_CAN_BE_DAMAGED(entity, FALSE);
                ENTITY::SET_ENTITY_VISIBLE(entity, TRUE);
                ENTITY::RESET_ENTITY_ALPHA(entity);
                if (!objectKind) {
                    PED::SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(ped, TRUE);
                    PED::SET_PED_KEEP_TASK(ped, TRUE);
                }
                if (!worldEntityReplicas_.Bind(
                        entityId,
                        static_cast<LocalEntityHandle>(entity))) {
                    if (objectKind) {
                        auto object = static_cast<Object>(entity);
                        OBJECT::DELETE_OBJECT(&object);
                    } else {
                        auto deletePed = ped;
                        PED::DELETE_PED(&deletePed);
                    }
                    entry.spawnDisposition =
                        WorldProxySpawnDisposition::RetryableFailure;
                    entry.spawnAttempts = std::min(
                        entry.spawnAttempts + 1U,
                        32U);
                    entry.nextSpawnRetryMs = now + 500U;
                    Log(
                        "[WARNING][ENTITY_SPAWN_RETRYABLE] local handle registry rejected entity " +
                        std::to_string(entityId.Value()) +
                        "; retry retained without desired-state despawn");
                    continue;
                }
                handle =
                    static_cast<LocalEntityHandle>(entity);
                entry.spawnDisposition =
                    WorldProxySpawnDisposition::Spawned;
                entry.spawnAttempts = 0U;
                entry.nextSpawnRetryMs = 0U;
                Log(
                    "world mirror proxy created for entity " +
                    std::to_string(entityId.Value()));
            }

            entry.spawnDisposition =
                WorldProxySpawnDisposition::Spawned;

            const auto entity = static_cast<Entity>(*handle);
            ENTITY::SET_ENTITY_VISIBLE(entity, TRUE);
            ENTITY::RESET_ENTITY_ALPHA(entity);
            ENTITY::SET_ENTITY_COLLISION(
                entity,
                entry.state.kind == WorldEntityKind::Object ? FALSE : TRUE,
                TRUE);
            if (IsOwnedHybridAnimSceneEntity(*handle)) {
                // Preserve visibility/collision, but never layer proxy
                // locomotion, heading snaps, health rewrites or semantic tasks
                // over an actor controlled by the exact native AnimScene.
                continue;
            }
            if (entry.state.kind == WorldEntityKind::Object) {
                ENTITY::SET_ENTITY_COORDS_NO_OFFSET(
                    entity,
                    entry.state.position.x,
                    entry.state.position.y,
                    entry.state.position.z,
                    FALSE,
                    FALSE,
                    FALSE);
                ENTITY::SET_ENTITY_HEADING(entity, entry.state.heading);
                ENTITY::SET_ENTITY_VELOCITY(
                    entity,
                    entry.state.velocity.x,
                    entry.state.velocity.y,
                    entry.state.velocity.z);
                continue;
            }
            const auto ped = static_cast<Ped>(entity);
            const auto currentPosition = ToBridgeVector(
                ENTITY::GET_ENTITY_COORDS(
                    ped,
                    TRUE,
                    FALSE));
            const auto mounted =
                (entry.state.flags &
                 static_cast<std::uint8_t>(
                     WorldEntityStateFlag::Mounted)) != 0U;
            const auto parentMount =
                mounted
                    ? worldEntityReplicas_.FindLocal(
                          entry.state.parentEntityId)
                    : std::nullopt;
            const bool mountedRelationReady =
                mounted &&
                parentMount.has_value() &&
                ENTITY::DOES_ENTITY_EXIST(
                    *parentMount) != FALSE;
            const auto ageSeconds =
                now >= entry.receivedAtMs
                    ? std::min(
                          static_cast<float>(
                              now - entry.receivedAtMs) /
                              1'000.0F,
                          0.25F)
                    : 0.0F;
            const Vec3 predicted{
                entry.state.position.x +
                    entry.state.velocity.x * ageSeconds,
                entry.state.position.y +
                    entry.state.velocity.y * ageSeconds,
                entry.state.position.z +
                    entry.state.velocity.z * ageSeconds};
            const auto error =
                Distance(currentPosition, predicted);
            const bool cinematicEntity =
                entry.state.taskKind == WorldTaskKind::Cinematic;
            if (mountedRelationReady) {
                if (PED::IS_PED_ON_MOUNT(ped) == FALSE ||
                    PED::GET_MOUNT(ped) !=
                        static_cast<Ped>(*parentMount)) {
                    SetPedOntoMount(
                        ped,
                        static_cast<Ped>(*parentMount));
                }
                entry.mounted = true;
            } else {
                if (entry.mounted &&
                    PED::IS_PED_ON_MOUNT(ped) != FALSE) {
                    TaskDismountAnimal(ped);
                }
                entry.mounted = false;
                const bool idleLike =
                    entry.state.taskKind ==
                        WorldTaskKind::Idle ||
                    entry.state.taskKind ==
                        WorldTaskKind::Scenario;
                if (std::isfinite(error) &&
                    ((cinematicEntity && error >= 0.03F) ||
                     error >=
                         kWorldProxySnapDistanceMeters ||
                     (idleLike && error >= 0.12F))) {
                    Vec3 corrected = predicted;
                    const auto hardCorrectionDistance =
                        cinematicEntity
                            ? 3.0F
                            : kWorldProxySnapDistanceMeters;
                    if (error < hardCorrectionDistance) {
                        const auto alpha =
                            1.0F -
                            std::exp(
                                -(cinematicEntity ? 8.0F : 10.0F) *
                                elapsedSeconds);
                        corrected = {
                            currentPosition.x +
                                (predicted.x -
                                 currentPosition.x) *
                                    alpha,
                            currentPosition.y +
                                (predicted.y -
                                 currentPosition.y) *
                                    alpha,
                            currentPosition.z +
                                (predicted.z -
                                 currentPosition.z) *
                                    alpha};
                    }
                    ENTITY::SET_ENTITY_COORDS_NO_OFFSET(
                        ped,
                        corrected.x,
                        corrected.y,
                        corrected.z,
                        FALSE,
                        FALSE,
                        FALSE);
                    if (cinematicEntity) {
                        ENTITY::SET_ENTITY_VELOCITY(
                            ped,
                            entry.state.velocity.x,
                            entry.state.velocity.y,
                            entry.state.velocity.z);
                        ENTITY::FORCE_ENTITY_AI_AND_ANIMATION_UPDATE(
                            ped,
                            FALSE);
                    }
                }
            }

            auto currentHeading =
                ENTITY::GET_ENTITY_HEADING(ped);
            auto headingDelta =
                std::fmod(
                    entry.state.heading -
                        currentHeading +
                        540.0F,
                    360.0F) -
                180.0F;
            if (!std::isfinite(currentHeading) ||
                !std::isfinite(headingDelta)) {
                currentHeading = entry.state.heading;
                headingDelta = 0.0F;
            }
            const auto headingAlpha =
                cinematicEntity
                    ? 1.0F -
                          std::exp(-10.0F * elapsedSeconds)
                    : 1.0F -
                          std::exp(-14.0F * elapsedSeconds);
            auto heading =
                currentHeading +
                headingDelta * headingAlpha;
            heading = std::fmod(heading, 360.0F);
            if (heading < 0.0F) {
                heading += 360.0F;
            }
            if (!mountedRelationReady) {
                ENTITY::SET_ENTITY_HEADING(ped, heading);
            }

            const auto dead =
                (entry.state.flags &
                 static_cast<std::uint8_t>(
                     WorldEntityStateFlag::Dead)) != 0U;
            const auto maximumHealth =
                std::max(
                    ENTITY::GET_ENTITY_MAX_HEALTH(
                        ped,
                        FALSE),
                    1);
            const auto targetHealth =
                dead
                    ? 0
                    : static_cast<int>(
                          std::lround(
                              entry.state.healthFraction *
                              static_cast<float>(
                                  maximumHealth)));
            const auto currentHealth =
                ENTITY::GET_ENTITY_HEALTH(ped);
            if (std::abs(
                    currentHealth -
                    targetHealth) > 1) {
                ENTITY::SET_ENTITY_HEALTH(
                    ped,
                    targetHealth,
                    0);
            }

            char unarmedName[] = "WEAPON_UNARMED";
            auto requestedWeapon =
                entry.state.weaponHash != 0U
                    ? static_cast<Hash>(
                          entry.state.weaponHash)
                    : GAMEPLAY::GET_HASH_KEY(
                          unarmedName);
            if (requestedWeapon != 0U &&
                WEAPON::IS_WEAPON_VALID(
                    requestedWeapon) != FALSE &&
                entry.weaponHash !=
                    static_cast<std::uint32_t>(
                        requestedWeapon)) {
                if (WEAPON::HAS_PED_GOT_WEAPON(
                        ped,
                        requestedWeapon,
                        FALSE,
                        FALSE) == FALSE) {
                    WEAPON::GIVE_DELAYED_WEAPON_TO_PED(
                        ped,
                        requestedWeapon,
                        24,
                        TRUE,
                        0);
                }
                WEAPON::SET_CURRENT_PED_WEAPON(
                    ped,
                    requestedWeapon,
                    TRUE,
                    0,
                    FALSE,
                    FALSE);
                entry.weaponHash =
                    static_cast<std::uint32_t>(
                        requestedWeapon);
            }

            const bool taskChanged =
                entry.previousTaskKind !=
                    entry.state.taskKind;
            const bool taskRefreshExpired =
                entry.previousTaskMs == 0U ||
                now < entry.previousTaskMs ||
                now - entry.previousTaskMs >=
                    (cinematicEntity
                         ? 250U
                         : kWorldSemanticTaskRefreshMilliseconds);
            const bool taskTargetChanged =
                Distance(
                    entry.previousTaskTarget,
                    entry.state.taskTarget) >=
                kWorldSemanticTaskDestinationRefreshMeters;
            if (!mountedRelationReady &&
                entry.state.taskKind !=
                    WorldTaskKind::Combat &&
                entry.state.taskKind !=
                    WorldTaskKind::Dead &&
                (taskChanged ||
                 taskRefreshExpired ||
                 taskTargetChanged)) {
                const auto speed = std::sqrt(
                    entry.state.velocity.x *
                        entry.state.velocity.x +
                    entry.state.velocity.y *
                        entry.state.velocity.y +
                    entry.state.velocity.z *
                        entry.state.velocity.z);
                switch (entry.state.taskKind) {
                    case WorldTaskKind::Locomotion:
                    case WorldTaskKind::Fleeing:
                        AI::TASK_GO_STRAIGHT_TO_COORD(
                            ped,
                            entry.state.taskTarget.x,
                            entry.state.taskTarget.y,
                            entry.state.taskTarget.z,
                            std::clamp(
                                speed,
                                1.0F,
                                6.0F),
                            2'500,
                            entry.state.heading,
                            0.2F,
                            0);
                        break;
                    case WorldTaskKind::Scenario:
                    case WorldTaskKind::Idle:
                        AI::TASK_STAND_STILL(
                            ped,
                            1'250);
                        break;
                    case WorldTaskKind::Cinematic:
                        if (speed > 0.20F) {
                            AI::TASK_GO_STRAIGHT_TO_COORD(
                                ped,
                                predicted.x + entry.state.velocity.x * 0.35F,
                                predicted.y + entry.state.velocity.y * 0.35F,
                                predicted.z + entry.state.velocity.z * 0.35F,
                                std::clamp(speed, 0.5F, 6.0F),
                                700,
                                entry.state.heading,
                                0.05F,
                                0);
                        } else {
                            AI::TASK_STAND_STILL(ped, 650);
                        }
                        break;
                    case WorldTaskKind::Mounted:
                    case WorldTaskKind::Combat:
                    case WorldTaskKind::Dead:
                        break;
                }
                PED::SET_PED_KEEP_TASK(ped, TRUE);
                entry.previousTaskKind =
                    entry.state.taskKind;
                entry.previousTaskTarget =
                    entry.state.taskTarget;
                entry.previousTaskMs = now;
            }

            const auto aiming =
                (entry.state.flags &
                 static_cast<std::uint8_t>(
                     WorldEntityStateFlag::Aiming)) != 0U;
            Entity aimTarget{};
            if (entry.state.combatTargetSlot ==
                WorldCombatTargetSlot::Guest) {
                aimTarget = PLAYER::PLAYER_PED_ID();
            } else if (
                entry.state.combatTargetSlot ==
                WorldCombatTargetSlot::Host) {
                aimTarget =
                    replicas_
                        .FindLocal(remotePlayerId_)
                        .value_or(0);
            }
            if (aiming &&
                aimTarget != 0 &&
                ENTITY::DOES_ENTITY_EXIST(
                    aimTarget) != FALSE &&
                (entry.previousAimTaskMs == 0U ||
                 now < entry.previousAimTaskMs ||
                 now - entry.previousAimTaskMs >=
                     kWorldAimTaskRefreshMilliseconds)) {
                // Deliberately use an aim-only task. Firing flags never create
                // local bullets, so a mirrored NPC cannot damage the guest.
                AI::TASK_AIM_GUN_AT_ENTITY(
                    ped,
                    aimTarget,
                    400,
                    FALSE,
                    0);
                entry.previousAimTaskMs = now;
            } else if (!aiming && entry.aiming) {
                AI::CLEAR_PED_SECONDARY_TASK(ped);
            }
            entry.aiming = aiming;
        }
        const auto localPed = PLAYER::PLAYER_PED_ID();
        if (localPed == 0 ||
            ENTITY::DOES_ENTITY_EXIST(localPed) == FALSE) {
            RestoreHiddenAmbientPeds();
            return;
        }
        const auto localPosition = ToBridgeVector(
            ENTITY::GET_ENTITY_COORDS(
                localPed,
                TRUE,
                FALSE));
        const auto remotePlayer =
            replicas_.FindLocal(remotePlayerId_);
        std::array<int, kWorldPedPoolCapacity> peds{};
        const auto count = std::clamp(
            worldGetAllPeds(
                peds.data(),
                static_cast<int>(peds.size())),
            0,
            static_cast<int>(peds.size()));
        std::unordered_set<LocalEntityHandle> shouldHide;
        shouldHide.reserve(
            static_cast<std::size_t>(count) +
            hiddenAmbientPeds_.size());
        // Hidden peds may disappear from ScriptHook's worldGetAllPeds result.
        // Seed the desired mask from our own stable handles first, otherwise
        // every other scan restores them and creates the observed 0/5 flicker.
        constexpr float kHiddenPopulationRadiusGraceMeters = 12.0F;
        if (authoritativePopulationReady) {
            for (const auto& [handle, entry] : hiddenAmbientPeds_) {
                const auto hiddenPed = static_cast<Ped>(handle);
                if (hiddenPed == 0 || hiddenPed == localPed ||
                    (remotePlayer.has_value() &&
                     hiddenPed == *remotePlayer) ||
                    ENTITY::DOES_ENTITY_EXIST(hiddenPed) == FALSE ||
                    static_cast<std::uint32_t>(
                        ENTITY::GET_ENTITY_MODEL(hiddenPed)) !=
                        entry.modelHash ||
                    replicas_.FindNetwork(hiddenPed).has_value() ||
                    remoteMountReplicas_.FindNetwork(hiddenPed).has_value() ||
                    worldEntityReplicas_.FindNetwork(hiddenPed).has_value()) {
                    continue;
                }
                const bool hiddenScriptOwned =
                    ENTITY::IS_ENTITY_A_MISSION_ENTITY(hiddenPed) != FALSE;
                int hiddenPedType{};
                bool hiddenUsedPedTypeFallback{};
                const bool hiddenReliableHuman =
                    IsPedHumanReliable(
                        hiddenPed,
                        hiddenPedType,
                        hiddenUsedPedTypeFallback);
                (void)hiddenPedType;
                (void)hiddenUsedPedTypeFallback;
                const bool hiddenHorse =
                    !hiddenReliableHuman &&
                    CanPedBeMounted(hiddenPed) != FALSE;
                const bool hiddenHuman =
                    hiddenReliableHuman ||
                    (hiddenScriptOwned && !hiddenHorse);
                const bool hiddenPlayerOwnedHorse =
                    hiddenHorse &&
                    GetPlayerOwnerOfMount(hiddenPed) ==
                        PLAYER::PLAYER_ID();
                const auto hiddenPopulationType =
                    ENTITY::_GET_ENTITY_POPULATION_TYPE(hiddenPed);
                const auto hiddenPosition = ToBridgeVector(
                    ENTITY::GET_ENTITY_COORDS(
                        hiddenPed,
                        TRUE,
                        FALSE));
                if ((!IsMirrorablePopulationType(hiddenPopulationType) &&
                     !hiddenScriptOwned) ||
                    (!hiddenHuman && !hiddenHorse) ||
                    hiddenPlayerOwnedHorse ||
                    !IsFinite(hiddenPosition) ||
                    Distance(localPosition, hiddenPosition) >
                        radiusMeters +
                            kHiddenPopulationRadiusGraceMeters) {
                    continue;
                }
                shouldHide.insert(handle);
            }
        }
        // Readiness comes from the authenticated host WorldState heartbeat,
        // not from the number of graph nodes. An empty graph is a meaningful
        // authoritative state (for example a mission interior or an empty
        // road) and must still suppress guest-local mission/ambient actors.
        std::size_t eligibleLocalPopulation{};
        std::size_t inRangeLocalPopulation{};
        std::size_t transparentLocalPopulation{};
        for (int index = 0;
             authoritativePopulationReady && index < count;
             ++index) {
            const auto ped = static_cast<Ped>(
                peds[static_cast<std::size_t>(index)]);
            if (ped == 0 ||
                ped == localPed ||
                (remotePlayer.has_value() &&
                 ped == *remotePlayer) ||
                 ENTITY::DOES_ENTITY_EXIST(ped) == FALSE ||
                 PED::IS_PED_A_PLAYER(ped) != FALSE ||
                 replicas_.FindNetwork(ped).has_value() ||
                 remoteMountReplicas_.FindNetwork(ped).has_value() ||
                 worldEntityReplicas_.FindNetwork(ped).has_value()) {
                continue;
            }
            const auto populationType =
                ENTITY::_GET_ENTITY_POPULATION_TYPE(ped);
            const bool scriptOwnedEntity =
                ENTITY::IS_ENTITY_A_MISSION_ENTITY(ped) != FALSE;
            int pedType{};
            bool usedPedTypeFallback{};
            const bool reliableHuman = IsPedHumanReliable(
                ped,
                pedType,
                usedPedTypeFallback);
            (void)pedType;
            (void)usedPedTypeFallback;
            const bool horse =
                !reliableHuman &&
                CanPedBeMounted(ped) != FALSE;
            const bool human =
                reliableHuman ||
                (scriptOwnedEntity && !horse);
            const bool playerOwnedHorse =
                horse &&
                GetPlayerOwnerOfMount(ped) ==
                    PLAYER::PLAYER_ID();
            if ((!IsMirrorablePopulationType(populationType) &&
                 !scriptOwnedEntity) ||
                 (!human && !horse) ||
                 playerOwnedHorse) {
                // The guest-owned horse remains local authority. Other
                // ambient humans and mountable horses are replaced by the
                // host mirror.
                continue;
            }
            ++eligibleLocalPopulation;
            const auto position = ToBridgeVector(
                ENTITY::GET_ENTITY_COORDS(
                    ped,
                    TRUE,
                    FALSE));
            if (Distance(localPosition, position) <=
                radiusMeters) {
                ++inRangeLocalPopulation;
                const bool visiblyPopulated =
                    ENTITY::IS_ENTITY_VISIBLE(ped) != FALSE ||
                    ENTITY::GET_ENTITY_ALPHA(ped) > 0;
                if (!visiblyPopulated) {
                    ++transparentLocalPopulation;
                }
                // An entity hidden by the authoritative mask is naturally
                // transparent on the next tick. It must remain in shouldHide;
                // otherwise the restore loop below makes it visible again and
                // produces an endless hide/show cycle for guest-local NPCs.
                shouldHide.insert(
                    static_cast<LocalEntityHandle>(
                        ped));
            }
        }

        for (auto iterator =
                 hiddenAmbientPeds_.begin();
             iterator != hiddenAmbientPeds_.end();) {
            const auto handle = iterator->first;
            const auto ped =
                static_cast<Ped>(handle);
            const bool sameEntity =
                ped != 0 &&
                ENTITY::DOES_ENTITY_EXIST(ped) != FALSE &&
                static_cast<std::uint32_t>(
                    ENTITY::GET_ENTITY_MODEL(ped)) ==
                    iterator->second.modelHash;
            if (sameEntity &&
                shouldHide.contains(handle) &&
                (IsMirrorablePopulationType(
                     ENTITY::_GET_ENTITY_POPULATION_TYPE(
                         ped)) ||
                 ENTITY::IS_ENTITY_A_MISSION_ENTITY(ped) != FALSE) &&
                ([&]() {
                    int pedType{};
                    bool usedPedTypeFallback{};
                    const bool human = IsPedHumanReliable(
                        ped,
                        pedType,
                        usedPedTypeFallback);
                    (void)pedType;
                    (void)usedPedTypeFallback;
                    return human ||
                           CanPedBeMounted(ped) != FALSE ||
                           ENTITY::IS_ENTITY_A_MISSION_ENTITY(ped) != FALSE;
                })()) {
                // Story/mission scripts can restore visibility and collision
                // after our first write. Reassert the non-destructive guest
                // world mask on every tick while host authority owns it.
                ENTITY::SET_ENTITY_VISIBLE(ped, FALSE);
                ENTITY::SET_ENTITY_ALPHA(ped, 0, FALSE);
                ENTITY::SET_ENTITY_COLLISION(
                    ped,
                    FALSE,
                    TRUE);
                ++iterator;
                continue;
            }
            if (sameEntity) {
                ENTITY::SET_ENTITY_VISIBLE(
                    ped,
                    iterator->second.wasVisible
                        ? TRUE
                        : FALSE);
                if (iterator->second.wasVisible) {
                    ENTITY::RESET_ENTITY_ALPHA(ped);
                }
                ENTITY::SET_ENTITY_COLLISION(
                    ped,
                    TRUE,
                    TRUE);
            }
            iterator =
                hiddenAmbientPeds_.erase(iterator);
        }
        for (const auto handle : shouldHide) {
            if (hiddenAmbientPeds_.contains(handle)) {
                continue;
            }
            const auto ped = static_cast<Ped>(handle);
            hiddenAmbientPeds_.emplace(
                handle,
                HiddenAmbientEntry{
                    static_cast<std::uint32_t>(
                        ENTITY::GET_ENTITY_MODEL(
                            ped)),
                    ENTITY::IS_ENTITY_VISIBLE(ped) != FALSE ||
                        ENTITY::GET_ENTITY_ALPHA(ped) > 0});
            ENTITY::SET_ENTITY_VISIBLE(ped, FALSE);
            ENTITY::SET_ENTITY_ALPHA(ped, 0, FALSE);
            ENTITY::SET_ENTITY_COLLISION(
                ped,
                FALSE,
                TRUE);
        }
        MaintainHiddenPedAttachments();
        if (previousWorldMirrorDiagnosticsMs_ == 0U ||
            now < previousWorldMirrorDiagnosticsMs_ ||
            now - previousWorldMirrorDiagnosticsMs_ >=
                kWorldMirrorDiagnosticsMilliseconds) {
            previousWorldMirrorDiagnosticsMs_ = now;
            std::size_t pendingModel{};
            std::size_t pendingDependency{};
            std::size_t retryableFailure{};
            std::size_t permanentFailure{};
            for (const auto& [entityId, entry] : worldProxyEntries_) {
                (void)entityId;
                switch (entry.spawnDisposition) {
                    case WorldProxySpawnDisposition::PendingModel:
                        ++pendingModel;
                        break;
                    case WorldProxySpawnDisposition::PendingDependency:
                        ++pendingDependency;
                        break;
                    case WorldProxySpawnDisposition::RetryableFailure:
                        ++retryableFailure;
                        break;
                    case WorldProxySpawnDisposition::PermanentFailure:
                        ++permanentFailure;
                        break;
                    case WorldProxySpawnDisposition::Spawned:
                        break;
                }
            }
            Log(
                "[WORLD_PROXY_PHYSICS][ENTITY_GRAPH_RENDER] v25 desired=" +
                std::to_string(worldProxyEntries_.size()) +
                ", spawned=" +
                std::to_string(worldEntityReplicas_.Size()) +
                ", pending-model=" +
                std::to_string(pendingModel) +
                ", pending-dependency=" +
                std::to_string(pendingDependency) +
                ", retryable-spawn-error=" +
                std::to_string(retryableFailure) +
                ", permanent-spawn-error=" +
                std::to_string(permanentFailure) +
                ", hidden-local-human=" +
                std::to_string(hiddenAmbientPeds_.size()) +
                ", local-pool=" +
                std::to_string(count) +
                ", local-eligible=" +
                std::to_string(eligibleLocalPopulation) +
                ", local-in-range=" +
                std::to_string(inRangeLocalPopulation) +
                ", local-transparent=" +
                std::to_string(transparentLocalPopulation) +
                ", authoritative-population-ready=" +
                (authoritativePopulationReady
                     ? std::string{"true"}
                     : std::string{"false"}) +
                ", guest-owned-horse-preserved=true");
        }
#else
        (void)active;
        (void)authoritativePopulationReady;
        (void)radiusMeters;
#endif
    } catch (...) {
        // Restore local world state immediately after any mirror failure.
        CleanupWorldEntityProxies();
        RestoreHiddenAmbientPeds();
        worldMirrorGuestActive_ = false;
    }
}

bool ScriptHookSdkFacade::ApplyWorldEntityDamage(
    const LocalEntityHandle target,
    const float damage) noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        const auto ped = static_cast<Ped>(target);
        if (ped == 0 ||
            ENTITY::DOES_ENTITY_EXIST(ped) == FALSE ||
            !std::isfinite(damage) ||
            damage <= 0.0F) {
            return false;
        }
        const auto clampedDamage =
            static_cast<int>(
                std::lround(
                    std::clamp(
                        damage,
                        1.0F,
                        kWorldDamageIntentFixedDamage)));
        PED::APPLY_DAMAGE_TO_PED(
            ped,
            clampedDamage,
            TRUE,
            0,
            0);
        return true;
#else
        (void)target;
        (void)damage;
#endif
    } catch (...) {
    }
    return false;
}

bool ScriptHookSdkFacade::ApplyMissionWorldEntityDamage(
    const LocalEntityHandle target,
    const std::uint32_t weaponHash,
    const float damage) noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        const auto victim = static_cast<Ped>(target);
        const auto attacker = replicas_.FindLocal(remotePlayerId_);
        if (victim == 0 ||
            !attacker.has_value() ||
            *attacker == 0 ||
            *attacker == victim ||
            ENTITY::DOES_ENTITY_EXIST(victim) == FALSE ||
            ENTITY::DOES_ENTITY_EXIST(*attacker) == FALSE ||
            PED::IS_PED_DEAD_OR_DYING(victim, TRUE) != FALSE ||
            weaponHash == 0U ||
            !std::isfinite(damage) ||
            damage <= 0.0F) {
            return false;
        }

        const auto source = ToBridgeVector(
            ENTITY::GET_ENTITY_COORDS(
                *attacker,
                TRUE,
                FALSE));
        const auto destination = ToBridgeVector(
            ENTITY::GET_ENTITY_COORDS(
                victim,
                TRUE,
                FALSE));
        if (!IsFinite(source) || !IsFinite(destination)) {
            return false;
        }

        const auto clampedDamage =
            static_cast<int>(
                std::lround(
                    std::clamp(
                        damage,
                        1.0F,
                        kWorldDamageIntentFixedDamage)));

        // Attribute the authoritative hit to the guest replica that lives in
        // the host process.  Mission scripts can then observe a real attacker
        // instead of anonymous APPLY_DAMAGE_TO_PED damage.  The client shot is
        // already rendered by the action lane, so this damage projectile is
        // intentionally inaudible and invisible to avoid a second tracer and
        // report while retaining collision and weapon attribution.
        GAMEPLAY::SHOOT_SINGLE_BULLET_BETWEEN_COORDS(
            source.x,
            source.y,
            source.z + 1.0F,
            destination.x,
            destination.y,
            destination.z + 0.85F,
            clampedDamage,
            TRUE,
            static_cast<Hash>(weaponHash),
            *attacker,
            FALSE,
            TRUE,
            500.0F,
            FALSE);
        return true;
#else
        (void)target;
        (void)weaponHash;
        (void)damage;
#endif
    } catch (...) {
    }
    return false;
}

void ScriptHookSdkFacade::MaintainRealtimeSession(
    const bool active,
    const bool synchronizedPaused) noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        if (active) {
            ObserveVanillaPickupCollection();
            ObserveScriptEvents();
        } else {
            observedVanillaPickups_.clear();
        }
        // Keep the bridge thread alive inside the real RDR2 frontend. Escape
        // is blocked before consensus; after both votes we inject the normal
        // frontend-pause control on both PCs so each opens the game's own
        // menu. Never fall back to SET_GAME_PAUSED(TRUE): opening the native
        // frontend can suspend the ScriptHook fiber long enough for that
        // timeout to expire after the player has already closed the menu,
        // which used to leave the local game permanently frozen.
        GAMEPLAY::SET_THIS_SCRIPT_CAN_BE_PAUSED(FALSE);
        const auto now = TickMilliseconds();
        const auto localPlayer = PLAYER::PLAYER_ID();
        if (active &&
            authoritativeLocalRestraint_ ==
                PlayerRestraintState::Lassoed &&
            authoritativeLocalRestraintReceivedMs_ != 0U &&
            (now < authoritativeLocalRestraintReceivedMs_ ||
             now - authoritativeLocalRestraintReceivedMs_ >
                 kAuthoritativeLassoLeaseMilliseconds)) {
            authoritativeLocalRestraint_ = PlayerRestraintState::Free;
            authoritativeLocalRestraintReceivedMs_ = 0U;
            previousAuthoritativeRestraintRagdollMs_ = 0U;
            DeleteRemotePeerLassoRope(
                "victim-restraint-lease-expired");
            const auto localPed = PLAYER::PLAYER_PED_ID();
            if (localPed != 0 &&
                ENTITY::DOES_ENTITY_EXIST(localPed) != FALSE &&
                !localDownedPolicyActive_) {
                AI::CLEAR_PED_TASKS_IMMEDIATELY(
                    localPed,
                    FALSE,
                    TRUE);
            }
            Log(
                "[WARNING][RESTRAINT_FSM] local lasso lease expired; restored player control once, target-only Sustain cannot recreate the native constraint until physical ownership returns");
        }
        if (active &&
            authoritativeLocalRestraint_ != PlayerRestraintState::Free) {
            const auto localPed = PLAYER::PLAYER_PED_ID();
            const bool nativeConstraintActive =
                localPed != 0 &&
                ENTITY::DOES_ENTITY_EXIST(localPed) != FALSE &&
                (IsPedLassoed(localPed) != FALSE ||
                 IsPedBeingHogtied(localPed) != FALSE ||
                 IsPedHogtied(localPed) != FALSE);
            const bool victimFallbackDue =
                previousAuthoritativeRestraintRagdollMs_ == 0U ||
                now < previousAuthoritativeRestraintRagdollMs_ ||
                now - previousAuthoritativeRestraintRagdollMs_ >=
                    kAuthoritativeRestraintRagdollRefreshMilliseconds;
            if (!nativeConstraintActive && victimFallbackDue &&
                localPed != 0 &&
                ENTITY::DOES_ENTITY_EXIST(localPed) != FALSE &&
                PED::SET_PED_TO_RAGDOLL(
                    localPed,
                    kAuthoritativeRestraintRagdollDurationMilliseconds,
                    kAuthoritativeRestraintRagdollDurationMilliseconds,
                    0,
                    TRUE,
                    TRUE,
                    FALSE) != FALSE) {
                previousAuthoritativeRestraintRagdollMs_ = now;
            }
        }
        const bool peerCombatIsolationRequested =
            active && peerCombatIsolationUntilMs_ != 0U &&
            now <= peerCombatIsolationUntilMs_;
        if (peerCombatIsolationRequested) {
            if (!peerCombatIsolationActive_) {
                peerCombatIsolationActive_ = true;
                peerCombatInitialWantedLevel_ =
                    PLAYER::GET_PLAYER_WANTED_LEVEL(localPlayer);
                Log(
                    "[INFO][PEER_COMBAT] NPC reaction and crime isolation enabled for peer-only combat");
            }
            // This guard is deliberately short-lived and refreshed only by a
            // peer melee/lasso target. It prevents ambient witnesses and honor
            // scripts from treating the private replica as a civilian victim,
            // without suppressing the host's ordinary combat indefinitely.
            PLAYER::SET_EVERYONE_IGNORE_PLAYER(localPlayer, TRUE);
            PLAYER::CLEAR_PLAYER_HAS_DAMAGED_AT_LEAST_ONE_PED(
                localPlayer);
            PLAYER::CLEAR_PLAYER_HAS_DAMAGED_AT_LEAST_ONE_NON_ANIMAL_PED(
                localPlayer);
            if (peerCombatInitialWantedLevel_ == 0 &&
                PLAYER::GET_PLAYER_WANTED_LEVEL(localPlayer) > 0) {
                PLAYER::CLEAR_PLAYER_WANTED_LEVEL(localPlayer);
            }
        } else if (peerCombatIsolationActive_) {
            PLAYER::SET_EVERYONE_IGNORE_PLAYER(localPlayer, FALSE);
            peerCombatIsolationActive_ = false;
            peerCombatInitialWantedLevel_ = 0;
            Log(
                "[INFO][PEER_COMBAT] NPC reaction and crime isolation released");
        }
        if (active) {
            if (synchronizedPaused) {
                if (!synchronizedPauseActive_) {
                    Log(
                        "synchronized coop pause accepted; requesting the native RDR2 frontend");
                    frontendPauseTogglePending_ = true;
                    frontendResumeTogglePending_ = false;
                    frontendPauseCycleCompleted_ = false;
                    frontendPauseToggleStartedMs_ = now;
                }
                synchronizedPauseActive_ = true;
                pauseOverrideActive_ = false;
                if (UI::IS_PAUSE_MENU_ACTIVE() != FALSE) {
                    if (frontendPauseTogglePending_) {
                        Log(
                            "native RDR2 pause frontend opened after coop consensus");
                    }
                    frontendPauseTogglePending_ = false;
                    // The real menu is open, but a single physical Escape
                    // must remain only a resume vote. Block the frontend
                    // control until host consensus requests the synthetic
                    // close below.
                    CONTROLS::DISABLE_CONTROL_ACTION(
                        kInputGroupGameplay,
                        kInputFrontendPause,
                        TRUE);
                    CONTROLS::DISABLE_CONTROL_ACTION(
                        kInputGroupGameplay,
                        kInputFrontendPauseAlternate,
                        TRUE);
                } else if (
                    frontendPauseTogglePending_ &&
                    now >= frontendPauseToggleStartedMs_ &&
                    now - frontendPauseToggleStartedMs_ <=
                        kFrontendPauseToggleTimeoutMilliseconds) {
                    (void)CONTROLS::_SET_CONTROL_NORMAL(
                        kInputGroupGameplay,
                        kInputFrontendPause,
                        1.0F);
                    (void)CONTROLS::_SET_CONTROL_NORMAL(
                        kInputGroupGameplay,
                        kInputFrontendPauseAlternate,
                        1.0F);
                } else if (frontendPauseTogglePending_) {
                    frontendPauseTogglePending_ = false;
                    frontendPauseCycleCompleted_ = true;
                    Log(
                        "native RDR2 pause frontend is no longer visible; refusing unsafe freeze fallback");
                    GAMEPLAY::SET_GAME_PAUSED(FALSE);
                    GAMEPLAY::SET_TIME_SCALE(1.0F);
                    UI::DISABLE_FRONTEND_THIS_FRAME();
                } else if (!frontendPauseCycleCompleted_) {
                    // We previously observed the frontend open. If execution
                    // resumes while it is no longer active, the player closed
                    // it locally before the authoritative resume arrived.
                    // Mark this vote cycle complete so it is never reopened
                    // or converted into a hard game freeze.
                    frontendPauseCycleCompleted_ = true;
                    Log(
                        "native RDR2 pause frontend closed locally; waiting for coop resume without freezing gameplay");
                    GAMEPLAY::SET_GAME_PAUSED(FALSE);
                    GAMEPLAY::SET_TIME_SCALE(1.0F);
                    UI::DISABLE_FRONTEND_THIS_FRAME();
                } else {
                    GAMEPLAY::SET_GAME_PAUSED(FALSE);
                    GAMEPLAY::SET_TIME_SCALE(1.0F);
                }
            } else {
                if (synchronizedPauseActive_) {
                    synchronizedPauseActive_ = false;
                    frontendPauseTogglePending_ = false;
                    frontendResumeTogglePending_ = true;
                    frontendPauseToggleStartedMs_ = now;
                    Log(
                        "synchronized coop resume accepted; closing the native RDR2 frontend");
                }
                frontendPauseCycleCompleted_ = false;

                if (frontendResumeTogglePending_) {
                    if (UI::IS_PAUSE_MENU_ACTIVE() != FALSE &&
                        now >= frontendPauseToggleStartedMs_ &&
                        now - frontendPauseToggleStartedMs_ <=
                            kFrontendPauseToggleTimeoutMilliseconds) {
                        (void)CONTROLS::_SET_CONTROL_NORMAL(
                            kInputGroupGameplay,
                            kInputFrontendPause,
                            1.0F);
                        (void)CONTROLS::_SET_CONTROL_NORMAL(
                            kInputGroupGameplay,
                            kInputFrontendPauseAlternate,
                            1.0F);
                    } else {
                        if (UI::IS_PAUSE_MENU_ACTIVE() != FALSE) {
                            Log(
                                "native RDR2 frontend did not close; forcing synchronized resume fallback");
                        } else {
                            Log(
                                "native RDR2 pause frontend closed after coop resume");
                        }
                        GAMEPLAY::SET_GAME_PAUSED(FALSE);
                        GAMEPLAY::SET_TIME_SCALE(1.0F);
                        UI::DISABLE_FRONTEND_THIS_FRAME();
                        frontendResumeTogglePending_ = false;
                    }
                } else {
                    CONTROLS::DISABLE_CONTROL_ACTION(
                        kInputGroupGameplay,
                        kInputFrontendPause,
                        TRUE);
                    CONTROLS::DISABLE_CONTROL_ACTION(
                        kInputGroupGameplay,
                        kInputFrontendPauseAlternate,
                        TRUE);
                    if (UI::IS_PAUSE_MENU_ACTIVE() != FALSE) {
                        GAMEPLAY::SET_GAME_PAUSED(FALSE);
                        UI::DISABLE_FRONTEND_THIS_FRAME();
                        if (!pauseOverrideActive_) {
                            Log(
                                "vanilla pause blocked until both coop players vote");
                        }
                        pauseOverrideActive_ = true;
                    } else {
                        pauseOverrideActive_ = false;
                    }
                    GAMEPLAY::SET_TIME_SCALE(1.0F);
                }
            }
        } else {
            // Stopping a session is also the emergency escape hatch. Always
            // release every time override even if bookkeeping was interrupted
            // while the native frontend had suspended this script.
            GAMEPLAY::SET_GAME_PAUSED(FALSE);
            GAMEPLAY::SET_TIME_SCALE(1.0F);
            synchronizedPauseActive_ = false;
            frontendPauseTogglePending_ = false;
            frontendResumeTogglePending_ = false;
            frontendPauseCycleCompleted_ = false;
            frontendPauseToggleStartedMs_ = 0U;
            pauseOverrideActive_ = false;
            peerCombatIsolationUntilMs_ = 0U;
            authoritativeLocalRestraint_ =
                PlayerRestraintState::Free;
            authoritativeLocalRestraintRevision_ = 0U;
            authoritativeLocalRestraintReceivedMs_ = 0U;
            previousAuthoritativeRestraintRagdollMs_ = 0U;
            DeleteRemotePeerLassoRope();
            if (worldClockWeatherOverrideActive_) {
                TIME::PAUSE_CLOCK(FALSE, 0);
                GAMEPLAY::FREEZE_WEATHER(FALSE);
                worldClockWeatherOverrideActive_ = false;
                Log(
                    "guest clock/weather override released");
            }
        }
#else
        (void)active;
        (void)synchronizedPaused;
#endif
        if (active != realtimePolicyActive_) {
            realtimePolicyActive_ = active;
            Log(
                active
                    ? "multiplayer real-time policy enabled"
                    : "multiplayer real-time policy disabled");
        }
    } catch (...) {
        // A best-effort time policy must never take down the script thread.
    }
}

void ScriptHookSdkFacade::ObserveVanillaPickupCollection() noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        const auto now = TickMilliseconds();
        if (previousPickupObservationMs_ != 0U && now >= previousPickupObservationMs_ &&
            now - previousPickupObservationMs_ < 250U) {
            return;
        }
        previousPickupObservationMs_ = now;

        constexpr int kMaximumPickups = 256;
        std::array<int, kMaximumPickups> pickups{};
        const auto count = std::clamp(worldGetAllPickups(pickups.data(), kMaximumPickups), 0, kMaximumPickups);
        std::unordered_set<int> live;
        for (int index = 0; index < count; ++index) {
            const auto pickup = static_cast<Pickup>(pickups[static_cast<std::size_t>(index)]);
            if (pickup == 0) continue;
            live.insert(static_cast<int>(pickup));
            const auto pickupHash = static_cast<std::uint32_t>(OBJECT::_GET_PICKUP_HASH(pickup));
            const auto coordinates = OBJECT::GET_PICKUP_COORDS(pickup);
            const auto quantize = [](const float value) noexcept {
                return static_cast<std::int32_t>(std::lround(value * 10.0F));
            };
            auto collectionId = std::uint64_t{1469598103934665603ULL};
            const auto mix = [&collectionId](const std::uint64_t value) noexcept {
                collectionId ^= value;
                collectionId *= 1099511628211ULL;
            };
            mix(pickupHash);
            mix(static_cast<std::uint32_t>(quantize(coordinates.x)));
            mix(static_cast<std::uint32_t>(quantize(coordinates.y)));
            mix(static_cast<std::uint32_t>(quantize(coordinates.z)));
            const auto [entry, firstSeen] = observedVanillaPickups_.try_emplace(static_cast<int>(pickup), collectionId);
            if (!firstSeen && OBJECT::HAS_PICKUP_BEEN_COLLECTED(pickup) != FALSE) {
                Log("[PICKUP_OBSERVED] collected handle=" + std::to_string(pickup) +
                    ", pickupHash=" + std::to_string(pickupHash) +
                    "; collection is positive-native evidence only; no private inventory copied");
                pendingVanillaPickupCollections_.push_back(
                    VanillaPickupCollection{
                        entry->second, pickupHash});
                observedVanillaPickups_.erase(entry);
            }
        }
        std::erase_if(observedVanillaPickups_, [&live](const auto& entry) {
            return !live.contains(entry.first);
        });
#endif
    } catch (...) {
    }
}

void ScriptHookSdkFacade::ObserveScriptEvents() noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        const auto now = TickMilliseconds();
        if (previousScriptEventObservationMs_ != 0U &&
            now >= previousScriptEventObservationMs_ &&
            now - previousScriptEventObservationMs_ < 1'000U) {
            return;
        }
        previousScriptEventObservationMs_ = now;
        // The SDK exposes script-event enumeration but not payload schemas.
        // Record only bounded group/index/type metadata until a specific
        // campaign event has been independently decoded and verified.
        constexpr int kEventGroups = 3;
        constexpr int kMaximumEventsPerGroup = 32;
        for (int group = 0; group < kEventGroups; ++group) {
            const auto count = std::clamp(
                SCRIPT::GET_NUMBER_OF_EVENTS(group), 0, kMaximumEventsPerGroup);
            for (int index = 0; index < count; ++index) {
                if (SCRIPT::GET_EVENT_EXISTS(group, index) == FALSE) continue;
                Log("[PROGRESSION_EVENT_OBSERVED] group=" +
                    std::to_string(group) + ", index=" +
                    std::to_string(index) + ", eventId=" +
                    std::to_string(SCRIPT::GET_EVENT_AT_INDEX(group, index)) +
                    "; diagnostic only, no capability emitted");
            }
        }
        // The official SDK exposes ownership checks but no supported
        // enumeration of a ped's entire weapon inventory. Poll only
        // individually proven capability records. A false-to-true edge proves
        // acquisition without requiring the player to equip the weapon.
        constexpr std::array<std::uint32_t, 1> kObservedWeaponCapabilities{
            kRepeatingShotgunWeaponHash};
        const auto ped = PLAYER::PLAYER_PED_ID();
        if (ped == 0 || ENTITY::DOES_ENTITY_EXIST(ped) == FALSE) {
            return;
        }
        for (const auto weaponHash : kObservedWeaponCapabilities) {
            const bool owned = WEAPON::HAS_PED_GOT_WEAPON(
                ped, static_cast<Hash>(weaponHash), FALSE, FALSE) != FALSE;
            const auto [iterator, inserted] =
                observedCampaignWeaponOwnership_.emplace(weaponHash, owned);
            if (inserted) {
                continue;
            }
            if (!iterator->second && owned) {
                pendingCampaignCapabilityObservations_.push_back(
                    CampaignCapabilityObservation{
                        CampaignCapabilityKind::WeaponShopEligibility,
                        weaponHash});
                Log("[CAPABILITY_OBSERVED] host acquired proven weapon capability hash=" +
                    std::to_string(weaponHash) +
                    "; equipment selection was not required");
            }
            iterator->second = owned;
        }
#endif
    } catch (...) {
    }
}

std::vector<VanillaPickupCollection>
ScriptHookSdkFacade::DrainVanillaPickupCollections() noexcept {
    auto result = std::move(pendingVanillaPickupCollections_);
    pendingVanillaPickupCollections_.clear();
    return result;
}

std::vector<CampaignCapabilityObservation>
ScriptHookSdkFacade::DrainCampaignCapabilityObservations() noexcept {
    auto result = std::move(pendingCampaignCapabilityObservations_);
    pendingCampaignCapabilityObservations_.clear();
    return result;
}

GuestMissionIsolationStatus
ScriptHookSdkFacade::MaintainMissionAuthority(
    const bool active,
    const bool hostMissionActive,
    const bool hostPresentationActive) noexcept {
    GuestMissionIsolationStatus status;
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        if (active) {
            const auto now = TickMilliseconds();
            const bool hostPresentationWasActive =
                guestHostPresentationActive_;
            guestHostMissionActive_ = hostMissionActive;
            guestHostPresentationActive_ = hostPresentationActive;
            if (hostPresentationWasActive && !hostPresentationActive) {
                guestMissionAnimSceneProbeCursor_ = 1;
                guestMissionAnimSceneProbeUntilMs_ =
                    now + kGuestMissionAnimSceneProbeBurstMilliseconds;
                guestMissionAnimSceneNextProbeMs_ = 0U;
            }
            if (!missionFlagOriginalCaptured_) {
                missionFlagOriginalValue_ =
                    GAMEPLAY::GET_MISSION_FLAG() != FALSE;
                missionFlagOriginalCaptured_ = true;
                guestMissionUnsafeAtAcquire_ =
                    missionFlagOriginalValue_;
                guestMissionIsolationDiagnosticsMs_ = 0U;
                if (guestMissionUnsafeAtAcquire_) {
                    guestMissionQuarantineActive_ = true;
                    guestMissionQuarantineUntilMs_ =
                        now +
                        kGuestMissionQuarantineMinimumMilliseconds;
                    guestMissionClearSinceMs_ = 0U;
                    ++guestMissionLocalTransitions_;
                    Log(
                        "[WARNING][MISSION_ISOLATION][MISSION_PREFLIGHT] vanilla mission gate was already occupied when the guest lease was acquired; JOIN must fail or remain quarantined");
                }
                Log(
                    "[MISSION_ISOLATION][MISSION_PREFLIGHT] guest Story gate reserved for the full authenticated lease; the nearby Story-actor input guard is the authoritative block because initialized marker visuals may remain yellow");
            }
            const bool missionGateWasAsserted =
                GAMEPLAY::GET_MISSION_FLAG() != FALSE;
            bool authoredSceneObserved{};
            for (auto iterator =
                     guestMissionQuarantinedAnimSceneHandles_.begin();
                 iterator !=
                 guestMissionQuarantinedAnimSceneHandles_.end();) {
                const auto candidate = *iterator;
                const bool sceneStillPresent =
                    candidate > 0 &&
                    candidate != ownedHybridAnimSceneHandle_ &&
                    DoesAnimSceneExist(candidate) != FALSE;
                const bool sceneStillPresenting =
                    sceneStillPresent &&
                    (IsAnimSceneRunning(candidate) != FALSE ||
                     GetAnimSceneActiveCameraCount(candidate) > 0);
                if (!sceneStillPresenting) {
                    iterator =
                        guestMissionQuarantinedAnimSceneHandles_.erase(
                            iterator);
                    continue;
                }
                authoredSceneObserved = true;
                guestMissionAuthoredSceneSeenUntilMs_ =
                    now + kGuestMissionClearConfirmationMilliseconds;
                ++iterator;
            }
            const auto suppressAuthoredScene = [&](const int candidate) {
                if (candidate <= 0 ||
                    candidate == ownedHybridAnimSceneHandle_ ||
                    DoesAnimSceneExist(candidate) == FALSE ||
                    IsAnimSceneRunning(candidate) == FALSE ||
                    GetAnimSceneActiveCameraCount(candidate) <= 0) {
                    return false;
                }
                // Never DELETE, pause, unpause, or accelerate a game-owned
                // scene: its Story script can be waiting for exact authored
                // timing. The previous unconditional unpause here could race
                // a script-owned pause and advance a private guest scene
                // independently of the host. Quarantine owns presentation
                // while the vanilla scene keeps its own native timing.
                guestMissionQuarantinedAnimSceneHandles_.insert(candidate);
                guestMissionAuthoredSceneSeenUntilMs_ =
                    now + kGuestMissionClearConfirmationMilliseconds;
                ++guestMissionSandboxOverrides_;
                return true;
            };
            const auto localPlayerForSceneGuard =
                static_cast<LocalEntityHandle>(
                    PLAYER::PLAYER_PED_ID());
            for (const auto candidate :
                 g_storyVmAnimSceneCapture.LoadedPlayerSceneHandles(
                     localPlayerForSceneGuard)) {
                if (candidate == ownedHybridAnimSceneHandle_) {
                    continue;
                }
                // LOAD precedes START in the captured Story VM sequence. Arm
                // spectator quarantine before the guest's private camera can
                // own even one rendered frame; START is still allowed and is
                // fast-forwarded below so the vanilla script can finish.
                guestMissionAuthoredSceneSeenUntilMs_ =
                    now + kGuestMissionClearConfirmationMilliseconds;
                authoredSceneObserved = true;
            }
            for (const auto candidate :
                 g_storyVmAnimSceneCapture.StartedSceneHandles()) {
                if (suppressAuthoredScene(candidate)) {
                    authoredSceneObserved = true;
                }
            }
            const bool authoredSceneBurstProbe =
                guestMissionAnimSceneProbeUntilMs_ != 0U &&
                now <= guestMissionAnimSceneProbeUntilMs_;
            const bool authoredScenePeriodicProbe =
                hostMissionActive &&
                (guestMissionAnimSceneNextProbeMs_ == 0U ||
                 now >= guestMissionAnimSceneNextProbeMs_);
            if (!authoredSceneObserved &&
                (authoredSceneBurstProbe || authoredScenePeriodicProbe)) {
                const auto probeBatchSize =
                    kAnimSceneProbeBatchSize * 2;
                guestMissionAnimSceneNextProbeMs_ =
                    now +
                    (authoredSceneBurstProbe
                         ? 16U
                         : kGuestMissionAnimScenePeriodicProbeMilliseconds);
                for (int attempt = 0;
                     attempt < probeBatchSize;
                     ++attempt) {
                    if (guestMissionAnimSceneProbeCursor_ <= 0 ||
                        guestMissionAnimSceneProbeCursor_ >
                            kAnimSceneMaximumProbeHandle) {
                        guestMissionAnimSceneProbeCursor_ = 1;
                    }
                    const auto candidate =
                        guestMissionAnimSceneProbeCursor_++;
                    if (suppressAuthoredScene(candidate)) {
                        authoredSceneObserved = true;
                        break;
                    }
                }
            }
            const bool localAuthoredSceneConflict =
                guestMissionAuthoredSceneSeenUntilMs_ != 0U &&
                now <= guestMissionAuthoredSceneSeenUntilMs_;
            const bool localCinematicConflict =
                localAuthoredSceneConflict ||
                (!hostPresentationActive &&
                 CAM::IS_CINEMATIC_CAM_RENDERING() != FALSE);
            // Losing player control is not sufficient evidence of a Story
            // mission. It also happens while Story Mode loads and on result
            // screens. V29.5 classified those transitions as fresh local
            // missions and repeatedly entered spectator/skip quarantine.
            const bool localControlConflict =
                !hostPresentationActive &&
                !missionSpectatorActive_ &&
                missionGateWasAsserted &&
                !missionFlagOverrideActive_ &&
                PLAYER::IS_PLAYER_CONTROL_ON(
                    PLAYER::PLAYER_ID()) == FALSE &&
                UI::IS_PAUSE_MENU_ACTIVE() == FALSE;
            const bool localSceneConflict =
                localCinematicConflict || localControlConflict;
            status.localMissionDetected =
                guestMissionUnsafeAtAcquire_ ||
                localSceneConflict;
            if (localSceneConflict) {
                if (!guestMissionLocalFlagPreviouslyDetected_) {
                    ++guestMissionLocalTransitions_;
                    Log(
                        std::string{
                            "[WARNING][MISSION_ISOLATION] guest entered a local scripted presentation outside the host scene; source="} +
                        (localAuthoredSceneConflict
                             ? "authored-animscene"
                             : localCinematicConflict
                                   ? "cinematic-camera"
                                   : "control-loss") +
                        ", presentation quarantined");
                }
                guestMissionQuarantineActive_ = true;
                guestMissionContaminated_ =
                    guestMissionContaminated_ || hostMissionActive;
                guestMissionQuarantineUntilMs_ =
                    now + kGuestMissionQuarantineMinimumMilliseconds;
                guestMissionClearSinceMs_ = 0U;
            } else if (guestMissionQuarantineActive_ &&
                       !guestMissionUnsafeAtAcquire_) {
                if (guestMissionClearSinceMs_ == 0U ||
                    now < guestMissionClearSinceMs_) {
                    guestMissionClearSinceMs_ = now;
                }
                const bool minimumHoldElapsed =
                    now >= guestMissionQuarantineUntilMs_;
                const bool clearConfirmed =
                    now >= guestMissionClearSinceMs_ &&
                    now - guestMissionClearSinceMs_ >=
                        kGuestMissionClearConfirmationMilliseconds;
                if (minimumHoldElapsed && clearConfirmed) {
                    guestMissionQuarantineActive_ = false;
                    guestMissionContaminated_ = false;
                    guestMissionQuarantineUntilMs_ = 0U;
                    guestMissionClearSinceMs_ = 0U;
                    Log(
                        hostMissionActive
                            ? "[MISSION_ISOLATION] residual guest-local scene cleared; quarantine released while the host mission gate remains reserved"
                            : "[MISSION_ISOLATION] guest-local mission flag remained clear; quarantine released");
                }
            }

            bool suppressLocalStoryPrompt{};
            LocalEntityHandle verifiedStoryActorHandle{};
            int verifiedStoryActorBlip{};
            const auto localPed = PLAYER::PLAYER_PED_ID();
            if (localPed != 0 &&
                ENTITY::DOES_ENTITY_EXIST(localPed) != FALSE) {
                const auto localPosition = ToBridgeVector(
                    ENTITY::GET_ENTITY_COORDS(
                        localPed,
                        TRUE,
                        FALSE));
                const auto isNearbyLocalStoryActor =
                    [&](const Ped candidate) {
                        if (candidate == 0 || candidate == localPed ||
                            ENTITY::DOES_ENTITY_EXIST(candidate) == FALSE ||
                            PED::IS_PED_A_PLAYER(candidate) != FALSE ||
                            ENTITY::IS_ENTITY_A_MISSION_ENTITY(candidate) == FALSE ||
                            replicas_.FindNetwork(candidate).has_value() ||
                            remoteMountReplicas_.FindNetwork(candidate).has_value() ||
                            worldEntityReplicas_.FindNetwork(candidate).has_value()) {
                            return false;
                        }
                        int pedType{};
                        bool usedPedTypeFallback{};
                        const bool human = IsPedHumanReliable(
                            candidate,
                            pedType,
                            usedPedTypeFallback);
                        (void)pedType;
                        (void)usedPedTypeFallback;
                        if (!human ||
                            CanPedBeMounted(candidate) != FALSE) {
                            return false;
                        }
                        const auto entityBlip =
                            RADAR::GET_BLIP_FROM_ENTITY(candidate);
                        if (entityBlip == 0 ||
                            RADAR::DOES_BLIP_EXIST(entityBlip) == FALSE) {
                            // IS_ENTITY_A_MISSION_ENTITY is far too broad in
                            // Story Mode: camp companions, conversation NPCs
                            // and temporary script-owned ambient peds all use
                            // it. Disabling context around every such ped made
                            // M2 talk and horse prompts unusable. A local
                            // mission-start guard now requires the actor's
                            // actual map/prompt blip as positive evidence.
                            return false;
                        }
                        const auto position = ToBridgeVector(
                            ENTITY::GET_ENTITY_COORDS(
                                candidate,
                                TRUE,
                                FALSE));
                        const bool insideGuard =
                            IsFinite(position) &&
                            Distance(localPosition, position) <=
                                kGuestStoryInteractionGuardRadiusMeters;
                        if (insideGuard) {
                            verifiedStoryActorHandle =
                                static_cast<LocalEntityHandle>(candidate);
                            verifiedStoryActorBlip = entityBlip;
                        }
                        return insideGuard;
                    };

                // Do not rely only on hiddenAmbientPeds_. A yellow-marker
                // actor can enter the local script pool one frame before the
                // reversible population mask has classified it. That gap let
                // a guest start their own Story transition in V26.1 even
                // though the vanilla mission flag was reserved. Scan the
                // complete local ped pool and close the context controls only
                // around a blipped non-mount mission actor; replicated host
                // actors, ordinary conversation NPCs, horses and both player
                // proxies remain interactive.
                std::array<int, kWorldPedPoolCapacity> storyPeds{};
                const auto storyPedCount = std::clamp(
                    worldGetAllPeds(
                        storyPeds.data(),
                        static_cast<int>(storyPeds.size())),
                    0,
                    static_cast<int>(storyPeds.size()));
                for (int index = 0;
                     index < storyPedCount &&
                     !suppressLocalStoryPrompt;
                     ++index) {
                    suppressLocalStoryPrompt =
                        isNearbyLocalStoryActor(
                            static_cast<Ped>(
                                storyPeds[static_cast<std::size_t>(
                                    index)]));
                }
                for (const auto& [handle, entry] : hiddenAmbientPeds_) {
                    (void)entry;
                    if (suppressLocalStoryPrompt ||
                        isNearbyLocalStoryActor(
                            static_cast<Ped>(handle))) {
                        suppressLocalStoryPrompt = true;
                        break;
                    }
                }
            }
            if (suppressLocalStoryPrompt) {
                ++guestMissionInteractionSuppressions_;
            }
            if (suppressLocalStoryPrompt !=
                guestMissionInteractionSuppressed_) {
                guestMissionInteractionSuppressed_ =
                    suppressLocalStoryPrompt;
                Log(
                    suppressLocalStoryPrompt
                        ? "[MISSION_ISOLATION] verified blipped guest mission actor inside 20m; targeted Story context guard active before marker activation; actor=" +
                              std::to_string(verifiedStoryActorHandle) +
                              ", blip=" +
                              std::to_string(verifiedStoryActorBlip)
                        : "[MISSION_ISOLATION] guest-local Story actor left the 20m guard");
            }
            status.localStoryInteractionSuppressed =
                suppressLocalStoryPrompt;
            if (suppressLocalStoryPrompt) {
                for (const auto control :
                     kGuestStoryContextControls) {
                    CONTROLS::DISABLE_CONTROL_ACTION(
                        kInputGroupGameplay,
                        control,
                        TRUE);
                }
                if (!hostMissionActive && !hostPresentationActive) {
                    DrawNativeRectangle(
                        0.79F,
                        0.925F,
                        0.38F,
                        0.044F,
                        45,
                        45,
                        45,
                        215);
                    DrawNativeText(
                        "COOP: MISSION BLOCKED FOR GUEST",
                        0.605F,
                        0.911F,
                        0.27F,
                        210,
                        210,
                        210,
                        255,
                        false);
                }
            }

            // Never fight an already-running Story VM by stopping its camera,
            // conversation or player control every frame. Those calls can race
            // AnimScene teardown and were the source of the V23 post-cutscene
            // ASI exception. The runtime consumes quarantineActive and keeps a
            // contaminated guest in the reversible spectator state instead.

            // MISSION_FLAG is process-global and also suppresses unrelated
            // vanilla conversations and mount prompts. Assert it only while a
            // positively identified mission-start actor, quarantine, or host
            // presentation needs the gate. The targeted context guard remains
            // the enforceable pre-marker block.
            const bool missionGateRequired =
                ShouldAssertGuestMissionGate(
                    active,
                    hostMissionActive,
                    hostPresentationActive,
                    guestMissionQuarantineActive_,
                    suppressLocalStoryPrompt);
            if (missionGateRequired && !missionFlagOverrideActive_) {
                // Own only the transition that this mod asserted. Writing
                // FALSE on every free-roam tick erased transient vanilla
                // conversation/prompt ownership and made the guest unable to
                // talk to NPCs even though no mission guard was active.
                missionFlagOverrideRestoreValue_ =
                    GAMEPLAY::GET_MISSION_FLAG() != FALSE;
                GAMEPLAY::SET_MISSION_FLAG(TRUE);
                missionFlagOverrideActive_ = true;
            } else if (!missionGateRequired &&
                       missionFlagOverrideActive_) {
                GAMEPLAY::SET_MISSION_FLAG(
                    missionFlagOverrideRestoreValue_ ? TRUE : FALSE);
                missionFlagOverrideActive_ = false;
            }
            status.missionGateAsserted = missionGateRequired;
            guestMissionLocalFlagPreviouslyDetected_ =
                localSceneConflict;
            status.quarantineActive =
                guestMissionQuarantineActive_;
            if (guestMissionIsolationDiagnosticsMs_ == 0U ||
                now < guestMissionIsolationDiagnosticsMs_ ||
                now - guestMissionIsolationDiagnosticsMs_ >=
                    kGuestMissionIsolationDiagnosticsMilliseconds) {
                guestMissionIsolationDiagnosticsMs_ = now;
                Log(
                    "[MISSION_ISOLATION] full-lease-guard vanilla-gate-before-assert=" +
                    std::to_string(missionGateWasAsserted ? 1 : 0) +
                    ", host-mission-protocol=" +
                    std::to_string(hostMissionActive ? 1 : 0) +
                    ", host-presentation=" +
                    std::to_string(hostPresentationActive ? 1 : 0) +
                    ", quarantine=" +
                    std::to_string(status.quarantineActive ? 1 : 0) +
                    ", unsafe-at-acquire=" +
                    std::to_string(
                        guestMissionUnsafeAtAcquire_ ? 1 : 0) +
                    ", transitions=" +
                    std::to_string(guestMissionLocalTransitions_) +
                    ", contaminated=" +
                    std::to_string(
                        guestMissionContaminated_ ? 1 : 0) +
                    ", sandbox-overrides=" +
                    std::to_string(
                        guestMissionSandboxOverrides_) +
                    ", local-hazard-recoveries=" +
                    std::to_string(
                        guestLocalHazardRecoveries_) +
                    ", prompt-guard=" +
                    std::to_string(
                        suppressLocalStoryPrompt ? 1 : 0) +
                    ", mission-gate-asserted=" +
                    std::to_string(
                        missionGateRequired ? 1 : 0) +
                    ", prompt-guard-ticks=" +
                    std::to_string(
                        guestMissionInteractionSuppressions_));
            }
            return status;
        }
        if (missionFlagOriginalCaptured_) {
            if (missionFlagOverrideActive_) {
                GAMEPLAY::SET_MISSION_FLAG(
                    missionFlagOverrideRestoreValue_ ? TRUE : FALSE);
                missionFlagOverrideActive_ = false;
            }
            GAMEPLAY::SET_MISSION_FLAG(
                missionFlagOriginalValue_
                    ? TRUE
                    : FALSE);
            missionFlagOverrideActive_ = false;
            missionFlagOriginalCaptured_ = false;
            Log(
                "[MISSION_ISOLATION] guest mission flag restored after session");
        }
        guestMissionLocalFlagPreviouslyDetected_ = false;
        guestMissionUnsafeAtAcquire_ = false;
        guestMissionQuarantineActive_ = false;
        guestMissionContaminated_ = false;
        guestMissionQuarantineUntilMs_ = 0U;
        guestMissionClearSinceMs_ = 0U;
        guestMissionIsolationDiagnosticsMs_ = 0U;
        guestMissionLocalTransitions_ = 0U;
        guestMissionInteractionSuppressed_ = false;
        guestMissionInteractionSuppressions_ = 0U;
        guestMissionSandboxOverrides_ = 0U;
        guestMissionAnimSceneProbeCursor_ = 1;
        guestMissionQuarantinedAnimSceneHandles_.clear();
        guestMissionAuthoredSceneSeenUntilMs_ = 0U;
        guestMissionAnimSceneProbeUntilMs_ = 0U;
        guestMissionAnimSceneNextProbeMs_ = 0U;
        guestHostMissionActive_ = false;
        guestHostPresentationActive_ = false;
        guestLocalHazardRecoveries_ = 0U;
#else
        (void)active;
        (void)hostMissionActive;
        (void)hostPresentationActive;
#endif
    } catch (...) {
        Log(
            "[ERROR][MISSION_ISOLATION] failed to maintain reversible guest mission gate");
    }
    return status;
}

MissionResumePreparation
ScriptHookSdkFacade::PrepareMissionCinematicResume(
    const Vec3& anchor,
    const float heading,
    const std::uint64_t nowMs) noexcept {
    MissionResumePreparation result;
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        if (!IsFinite(anchor) || !std::isfinite(heading)) {
            result.ready = true;
            result.fallbackUsed = true;
            missionResumeFallbackUsed_ = true;
            missionDeferredResumePending_ = false;
            return result;
        }
        Vec3 effectiveAnchor = anchor;
        float effectiveHeading = heading;
        bool usedLiveHostAnchor{};
        if (const auto remoteHost = replicas_.FindLocal(remotePlayerId_);
            remoteHost.has_value() &&
            ENTITY::DOES_ENTITY_EXIST(*remoteHost) != FALSE) {
            const auto livePosition = ToBridgeVector(
                ENTITY::GET_ENTITY_COORDS(*remoteHost, TRUE, FALSE));
            const auto liveHeading = ENTITY::GET_ENTITY_HEADING(*remoteHost);
            if (IsFinite(livePosition) && std::isfinite(liveHeading)) {
                effectiveAnchor = livePosition;
                effectiveHeading = liveHeading;
                usedLiveHostAnchor = true;
            }
        }
        const bool firstAnchor = !missionResumePreparing_;
        const bool newAnchor =
            firstAnchor ||
            Distance(missionResumeAnchor_, effectiveAnchor) > 0.25F;
        if (newAnchor) {
            auto normalizedHeading = std::fmod(effectiveHeading, 360.0F);
            if (normalizedHeading < 0.0F) {
                normalizedHeading += 360.0F;
            }
            const auto radians =
                normalizedHeading *
                (std::numbers::pi_v<float> / 180.0F);
            const Vec3 right{std::cos(radians), -std::sin(radians), 0.0F};
            const Vec3 forward{std::sin(radians), std::cos(radians), 0.0F};
            missionResumePosition_ = TeleportSafePosition(
                {effectiveAnchor.x + (right.x * 1.5F),
                 effectiveAnchor.y + (right.y * 1.5F),
                 effectiveAnchor.z + 0.25F});
            missionResumeMountPosition_ = TeleportSafePosition(
                {effectiveAnchor.x - (right.x * 1.75F) - forward.x,
                 effectiveAnchor.y - (right.y * 1.75F) - forward.y,
                 effectiveAnchor.z + 0.25F});
            missionResumeHeading_ = normalizedHeading;
            missionResumeAnchor_ = effectiveAnchor;
            missionResumeRequestedAtMs_ = nowMs;
            missionResumePreparing_ = true;
            missionResumePrepared_ = false;
            missionResumeFallbackUsed_ = false;
            missionStreamingFocus_ = effectiveAnchor;
            if (firstAnchor) {
                Log(
                    std::string{"[MISSION_RESUME] preparing from "} +
                    (usedLiveHostAnchor
                         ? "fresh remote-host position"
                         : "host wire anchor") +
                    "; wire-drift-m=" +
                    std::to_string(Distance(anchor, effectiveAnchor)));
            }
        }
        if (CAM::IS_SCREEN_FADED_OUT() == FALSE &&
            CAM::IS_SCREEN_FADING_OUT() == FALSE) {
            CAM::DO_SCREEN_FADE_OUT(150);
        }
        STREAMING::REQUEST_COLLISION_AT_COORD(
            missionResumePosition_.x,
            missionResumePosition_.y,
            missionResumePosition_.z);
        STREAMING::REQUEST_COLLISION_AT_COORD(
            missionResumeMountPosition_.x,
            missionResumeMountPosition_.y,
            missionResumeMountPosition_.z);
        STREAMING::_SET_FOCUS_AREA(
            effectiveAnchor.x,
            effectiveAnchor.y,
            effectiveAnchor.z,
            0.0F,
            0.0F,
            0.0F);
        STREAMING::SET_HD_AREA(
            effectiveAnchor.x,
            effectiveAnchor.y,
            effectiveAnchor.z,
            kMissionCinematicStreamingRadiusMeters);
        missionStreamingFocusActive_ = true;
        const auto elapsed = nowMs >= missionResumeRequestedAtMs_
                                 ? nowMs - missionResumeRequestedAtMs_
                                 : 0U;
        if (elapsed >= kMissionResumeStreamingWarmupMilliseconds) {
            missionResumePrepared_ = true;
            result.ready = true;
            return result;
        }
        if (elapsed >= kMissionResumeFallbackMilliseconds) {
            missionResumeFallbackUsed_ = true;
            missionDeferredResumePending_ = true;
            result.ready = true;
            result.fallbackUsed = true;
        }
#else
        (void)anchor;
        (void)heading;
        (void)nowMs;
        result.ready = true;
#endif
    } catch (...) {
        missionResumeFallbackUsed_ = true;
        missionDeferredResumePending_ = true;
        result.ready = true;
        result.fallbackUsed = true;
        Log(
            "[ERROR][MISSION_RESUME] streaming preparation failed; using saved pre-scene position");
    }
    return result;
}

bool ScriptHookSdkFacade::IsCutsceneSkipPressed() noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        return CONTROLS::IS_CONTROL_JUST_PRESSED(
                   kInputGroupGameplay,
                   kInputSkipCutscene) != FALSE ||
               CONTROLS::IS_DISABLED_CONTROL_JUST_PRESSED(
                   kInputGroupGameplay,
                   kInputSkipCutscene) != FALSE;
#endif
    } catch (...) {
    }
    return false;
}

void ScriptHookSdkFacade::MaintainCutsceneSkipInput(
    const bool active) noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        if (active != cutsceneSkipInputActive_) {
            cutsceneSkipInputActive_ = active;
            Log(
                active
                    ? "[MISSION_SKIP][INPUT] standard INPUT_SKIP_CUTSCENE injection active in this RDR2 process"
                    : "[MISSION_SKIP][INPUT] standard INPUT_SKIP_CUTSCENE injection released in this RDR2 process");
        }
        if (active) {
            (void)CONTROLS::_SET_CONTROL_NORMAL(
                kInputGroupGameplay,
                kInputSkipCutscene,
                1.0F);
        }
#else
        (void)active;
#endif
    } catch (...) {
        Log("[ERROR][MISSION_SKIP] could not feed INPUT_SKIP_CUTSCENE");
    }
}

void ScriptHookSdkFacade::MaintainMissionSpectator(
    const bool active) noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        const auto localPed = PLAYER::PLAYER_PED_ID();
        const auto remoteHost = replicas_.FindLocal(remotePlayerId_);
        if (active) {
            if (localPed == 0 ||
                ENTITY::DOES_ENTITY_EXIST(localPed) == FALSE) {
                return;
            }
            if (!missionSpectatorActive_) {
                missionSpectatorPed_ =
                    static_cast<LocalEntityHandle>(localPed);
                missionSpectatorWasVisible_ =
                    ENTITY::IS_ENTITY_VISIBLE(localPed) != FALSE ||
                    ENTITY::GET_ENTITY_ALPHA(localPed) > 0;
                missionSpectatorSavedPosition_ = ToBridgeVector(
                    ENTITY::GET_ENTITY_COORDS(localPed, TRUE, FALSE));
                missionSpectatorSavedHeading_ =
                    ENTITY::GET_ENTITY_HEADING(localPed);
                missionSpectatorMount_ = 0;
                missionSpectatorMountWasVisible_ = true;
                if (PED::IS_PED_ON_MOUNT(localPed) != FALSE) {
                    const auto mount = PED::GET_MOUNT(localPed);
                    if (mount != 0 &&
                        ENTITY::DOES_ENTITY_EXIST(mount) != FALSE) {
                        missionSpectatorMount_ =
                            static_cast<LocalEntityHandle>(mount);
                        missionSpectatorMountWasVisible_ =
                            ENTITY::IS_ENTITY_VISIBLE(mount) != FALSE ||
                            ENTITY::GET_ENTITY_ALPHA(mount) > 0;
                    }
                }
                missionResumePreparing_ = false;
                missionResumePrepared_ = false;
                missionResumeFallbackUsed_ = false;
                missionDeferredResumePending_ = false;
                missionResumeRequestedAtMs_ = 0U;
                missionDeferredResumeRetryMs_ = 0U;
                missionSpectatorActive_ = true;
                missionSpectatorStartedAtMs_ = TickMilliseconds();
                const Vec3 stagingPosition{
                    missionSpectatorSavedPosition_.x,
                    missionSpectatorSavedPosition_.y,
                    missionSpectatorSavedPosition_.z +
                        kMissionSpectatorStoryTriggerSeparationMeters};
                if (missionSpectatorMount_ != 0 &&
                    ENTITY::DOES_ENTITY_EXIST(
                        missionSpectatorMount_) != FALSE) {
                    ENTITY::SET_ENTITY_COORDS_NO_OFFSET(
                        missionSpectatorMount_,
                        stagingPosition.x,
                        stagingPosition.y,
                        stagingPosition.z,
                        FALSE,
                        FALSE,
                        FALSE);
                } else {
                    ENTITY::SET_ENTITY_COORDS_NO_OFFSET(
                        localPed,
                        stagingPosition.x,
                        stagingPosition.y,
                        stagingPosition.z,
                        FALSE,
                        FALSE,
                        FALSE);
                }
                if (CAM::IS_SCREEN_FADED_OUT() == FALSE &&
                    CAM::IS_SCREEN_FADING_OUT() == FALSE) {
                    CAM::DO_SCREEN_FADE_OUT(150);
                }
                Log(
                    "[MISSION_SPECTATOR] native guest freeze/hide armed and the invisible local Story actor staged outside the mission trigger; waiting up to 1500ms for the first host camera keyframe");
            }

            PLAYER::SET_PLAYER_CONTROL(
                PLAYER::PLAYER_ID(),
                FALSE,
                0,
                FALSE);
            ENTITY::FREEZE_ENTITY_POSITION(localPed, TRUE);
            ENTITY::SET_ENTITY_INVINCIBLE(localPed, TRUE);
            ENTITY::SET_ENTITY_VISIBLE(localPed, FALSE);
            ENTITY::SET_ENTITY_ALPHA(localPed, 0, FALSE);
            ENTITY::SET_ENTITY_COLLISION(localPed, FALSE, TRUE);
            if (missionSpectatorMount_ != 0 &&
                ENTITY::DOES_ENTITY_EXIST(
                    missionSpectatorMount_) != FALSE) {
                ENTITY::FREEZE_ENTITY_POSITION(
                    missionSpectatorMount_,
                    TRUE);
                ENTITY::SET_ENTITY_INVINCIBLE(
                    missionSpectatorMount_,
                    TRUE);
                ENTITY::SET_ENTITY_VISIBLE(
                    missionSpectatorMount_,
                    FALSE);
                ENTITY::SET_ENTITY_ALPHA(
                    missionSpectatorMount_,
                    0,
                    FALSE);
                ENTITY::SET_ENTITY_COLLISION(
                    missionSpectatorMount_,
                    FALSE,
                    TRUE);
            }

            Vec3 presentationFocus = missionSpectatorSavedPosition_;
            if (missionNativeAnimSceneActive_) {
                presentationFocus = missionNativeAnimSceneOrigin_;
            } else if (missionReplicatedCameraActive_) {
                presentationFocus = missionReplicatedCameraPosition_;
            } else if (remoteHost.has_value() &&
                       ENTITY::DOES_ENTITY_EXIST(*remoteHost) != FALSE) {
                presentationFocus = ToBridgeVector(
                    ENTITY::GET_ENTITY_COORDS(*remoteHost, TRUE, FALSE));
            }
            if (IsFinite(presentationFocus)) {
                STREAMING::_SET_FOCUS_AREA(
                    presentationFocus.x,
                    presentationFocus.y,
                    presentationFocus.z,
                    0.0F,
                    0.0F,
                    0.0F);
                STREAMING::SET_HD_AREA(
                    presentationFocus.x,
                    presentationFocus.y,
                    presentationFocus.z,
                    kMissionCinematicStreamingRadiusMeters);
                missionStreamingFocus_ = presentationFocus;
                missionStreamingFocusActive_ = true;
            }

            // A matching local AnimScene owns the exact cast and therefore
            // keeps transform-driven host proxies hidden. When no such scene
            // exists, V30.3 uses a deliberately limited kinematic cast: the
            // host proxies are visible, non-colliding and driven by smoothed
            // roots plus native idle/locomotion tasks. This cannot invent the
            // authored dialogue gestures, but avoids both an empty frame and
            // the direct 30 Hz snap/T-pose loop observed in V29.5/V30.2.
            const bool proxyCastFallback =
                !missionNativeAnimSceneActive_;
            if (proxyCastFallback !=
                missionProxyCastFallbackActive_) {
                missionProxyCastFallbackActive_ = proxyCastFallback;
                Log(
                    proxyCastFallback
                        ? "[ANIMSCENE_REPLICA][PROXY_CAST_FALLBACK] no local scene resource; showing non-colliding host cast with smoothed roots and native idle/locomotion graphs"
                        : "[ANIMSCENE_REPLICA][PROXY_CAST_FALLBACK] exact local AnimScene attached; kinematic host cast hidden");
            }
            for (const auto& [entityId, entry] : worldProxyEntries_) {
                (void)entry;
                const auto proxy =
                    worldEntityReplicas_.FindLocal(entityId);
                if (!proxy.has_value() ||
                    ENTITY::DOES_ENTITY_EXIST(*proxy) == FALSE) {
                    continue;
                }
                ENTITY::SET_ENTITY_COLLISION(*proxy, FALSE, TRUE);
                if (proxyCastFallback) {
                    ENTITY::FREEZE_ENTITY_POSITION(*proxy, FALSE);
                    ENTITY::SET_ENTITY_VISIBLE(*proxy, TRUE);
                    ENTITY::RESET_ENTITY_ALPHA(*proxy);
                    ENTITY::FORCE_ENTITY_AI_AND_ANIMATION_UPDATE(
                        *proxy,
                        FALSE);
                } else if (IsOwnedHybridAnimSceneEntity(*proxy)) {
                    // Exact scenes bind the already replicated world actors;
                    // they do not spawn a second cast. V31.2 hid every world
                    // proxy as soon as the native scene became active, which
                    // made a correctly bound scene camera render without its
                    // Dutch/Hosea cast. Keep only bound actors visible and let
                    // the AnimScene own their animation and root motion.
                    ENTITY::FREEZE_ENTITY_POSITION(*proxy, FALSE);
                    ENTITY::SET_ENTITY_VISIBLE(*proxy, TRUE);
                    ENTITY::RESET_ENTITY_ALPHA(*proxy);
                    ENTITY::FORCE_ENTITY_AI_AND_ANIMATION_UPDATE(
                        *proxy,
                        TRUE);
                } else {
                    ENTITY::FREEZE_ENTITY_POSITION(*proxy, TRUE);
                    ENTITY::SET_ENTITY_VISIBLE(*proxy, FALSE);
                    ENTITY::SET_ENTITY_ALPHA(*proxy, 0, FALSE);
                }
            }

            const auto localPosition = missionStreamingFocusActive_
                                           ? missionStreamingFocus_
                                           : ToBridgeVector(
                                                 ENTITY::GET_ENTITY_COORDS(
                                                     localPed,
                                                     TRUE,
                                                     FALSE));
            if (missionNativeAnimSceneActive_) {
                // A dictionary/duration-matched local AnimScene owns its cast,
                // voice tracks and subtitles. Restore the reversible ambient
                // mask so that engine-owned scene actors remain visible.
                RestoreHiddenAmbientPeds();
            } else {
                std::array<int, kWorldPedPoolCapacity> scenePeds{};
                const auto scenePedCount = std::clamp(
                    worldGetAllPeds(
                        scenePeds.data(),
                        static_cast<int>(scenePeds.size())),
                    0,
                    static_cast<int>(scenePeds.size()));
                for (int index = 0; index < scenePedCount; ++index) {
                const auto ped = static_cast<Ped>(
                    scenePeds[static_cast<std::size_t>(index)]);
                if (ped == 0 || ped == localPed ||
                    (remoteHost.has_value() && ped == *remoteHost) ||
                    ENTITY::DOES_ENTITY_EXIST(ped) == FALSE ||
                    PED::IS_PED_A_PLAYER(ped) != FALSE ||
                    replicas_.FindNetwork(ped).has_value() ||
                    remoteMountReplicas_.FindNetwork(ped).has_value() ||
                    worldEntityReplicas_.FindNetwork(ped).has_value()) {
                    continue;
                }
                int pedType{};
                bool usedPedTypeFallback{};
                const bool human = IsPedHumanReliable(
                    ped,
                    pedType,
                    usedPedTypeFallback);
                (void)pedType;
                (void)usedPedTypeFallback;
                const bool horse =
                    !human && CanPedBeMounted(ped) != FALSE;
                const bool scriptOwned =
                    ENTITY::IS_ENTITY_A_MISSION_ENTITY(ped) != FALSE;
                const bool localOwnedHorse =
                    horse &&
                    GetPlayerOwnerOfMount(ped) == PLAYER::PLAYER_ID();
                const auto position = ToBridgeVector(
                    ENTITY::GET_ENTITY_COORDS(ped, TRUE, FALSE));
                if ((!human && !horse && !scriptOwned) ||
                    localOwnedHorse || !IsFinite(position) ||
                    Distance(localPosition, position) >
                        kMissionSpectatorPopulationRadiusMeters) {
                    continue;
                }
                const auto handle =
                    static_cast<LocalEntityHandle>(ped);
                if (!hiddenAmbientPeds_.contains(handle)) {
                    hiddenAmbientPeds_.emplace(
                        handle,
                        HiddenAmbientEntry{
                            static_cast<std::uint32_t>(
                                ENTITY::GET_ENTITY_MODEL(ped)),
                            ENTITY::IS_ENTITY_VISIBLE(ped) != FALSE ||
                                ENTITY::GET_ENTITY_ALPHA(ped) > 0});
                }
                }
                for (const auto& [handle, entry] : hiddenAmbientPeds_) {
                const auto ped = static_cast<Ped>(handle);
                if (ped == 0 ||
                    ENTITY::DOES_ENTITY_EXIST(ped) == FALSE ||
                    static_cast<std::uint32_t>(
                        ENTITY::GET_ENTITY_MODEL(ped)) != entry.modelHash) {
                    continue;
                }
                ENTITY::SET_ENTITY_VISIBLE(ped, FALSE);
                ENTITY::SET_ENTITY_ALPHA(ped, 0, FALSE);
                    ENTITY::SET_ENTITY_COLLISION(ped, FALSE, TRUE);
                }
                MaintainHiddenPedAttachments();
            }

            const auto presentationNow = TickMilliseconds();
            const bool initialCameraGraceElapsed =
                missionSpectatorStartedAtMs_ == 0U ||
                presentationNow < missionSpectatorStartedAtMs_ ||
                presentationNow - missionSpectatorStartedAtMs_ >=
                    kMissionInitialCameraGraceMilliseconds;
            if (!missionNativeAnimSceneActive_ &&
                missionSpectatorCamera_ == 0 &&
                initialCameraGraceElapsed &&
                remoteHost.has_value() &&
                ENTITY::DOES_ENTITY_EXIST(*remoteHost) != FALSE) {
                const auto hostPosition = ToBridgeVector(
                    ENTITY::GET_ENTITY_COORDS(
                        *remoteHost,
                        TRUE,
                        FALSE));
                char cameraName[] = "DEFAULT_SCRIPTED_CAMERA";
                missionSpectatorCamera_ = CAM::CREATE_CAM_WITH_PARAMS(
                    cameraName,
                    hostPosition.x,
                    hostPosition.y - 4.0F,
                    hostPosition.z + 2.0F,
                    0.0F,
                    0.0F,
                    0.0F,
                    55.0F,
                    TRUE,
                    2);
                if (missionSpectatorCamera_ != 0 &&
                    CAM::DOES_CAM_EXIST(
                        missionSpectatorCamera_) != FALSE) {
                    CAM::ATTACH_CAM_TO_ENTITY(
                        missionSpectatorCamera_,
                        *remoteHost,
                        0.0F,
                        -4.0F,
                        2.0F,
                        TRUE);
                    CAM::POINT_CAM_AT_ENTITY(
                        missionSpectatorCamera_,
                        *remoteHost,
                        0.0F,
                        0.0F,
                        0.8F,
                        TRUE);
                    CAM::SET_CAM_ACTIVE(
                        missionSpectatorCamera_,
                        TRUE);
                    CAM::RENDER_SCRIPT_CAMS(
                        TRUE,
                        TRUE,
                        350,
                        TRUE,
                        FALSE,
                        0);
                }
            }
            if (missionSpectatorCamera_ != 0 &&
                CAM::DOES_CAM_EXIST(missionSpectatorCamera_) != FALSE &&
                !missionNativeAnimSceneActive_ &&
                !missionReplicatedCameraActive_ &&
                !missionResumePreparing_) {
                // A late guest-local Story scene may attempt to reclaim the
                // rendering stack every frame. Reassert the reversible host
                // spectator camera so that scene stays quarantined instead of
                // appearing as a sequential second cutscene.
                CAM::SET_CAM_ACTIVE(missionSpectatorCamera_, TRUE);
                CAM::RENDER_SCRIPT_CAMS(
                    TRUE,
                    FALSE,
                    0,
                    TRUE,
                    FALSE,
                    0);
            }
            if (missionSpectatorCamera_ != 0 &&
                !missionReplicatedCameraActive_ &&
                !missionResumePreparing_ &&
                CAM::IS_SCREEN_FADED_OUT() != FALSE &&
                CAM::IS_SCREEN_FADING_IN() == FALSE) {
                CAM::DO_SCREEN_FADE_IN(250);
            }
            return;
        }

        if (missionStreamingFocusActive_) {
            STREAMING::CLEAR_FOCUS();
            STREAMING::CLEAR_HD_AREA();
            missionStreamingFocusActive_ = false;
            missionStreamingFocus_ = {};
        }
        if (!missionSpectatorActive_ &&
            missionDeferredResumePending_) {
            const auto now = TickMilliseconds();
            STREAMING::REQUEST_COLLISION_AT_COORD(
                missionResumePosition_.x,
                missionResumePosition_.y,
                missionResumePosition_.z);
            if (now >= missionDeferredResumeRetryMs_ &&
                localPed != 0 &&
                ENTITY::DOES_ENTITY_EXIST(localPed) != FALSE) {
                ENTITY::SET_ENTITY_COORDS_NO_OFFSET(
                    localPed,
                    missionResumePosition_.x,
                    missionResumePosition_.y,
                    missionResumePosition_.z,
                    FALSE,
                    FALSE,
                    FALSE);
                ENTITY::SET_ENTITY_HEADING(
                    localPed,
                    missionResumeHeading_);
                missionDeferredResumePending_ = false;
                missionResumePreparing_ = false;
                missionResumePrepared_ = false;
                missionResumeFallbackUsed_ = false;
                Log(
                    "[MISSION_RESUME] deferred post-fallback teleport completed");
            }
        }
        if (!missionSpectatorActive_ &&
            missionSpectatorCamera_ == 0) {
            missionProxyCastFallbackActive_ = false;
            return;
        }
        if (missionSpectatorCamera_ != 0) {
            CAM::RENDER_SCRIPT_CAMS(
                FALSE,
                TRUE,
                350,
                TRUE,
                FALSE,
                0);
            if (CAM::DOES_CAM_EXIST(
                    missionSpectatorCamera_) != FALSE) {
                CAM::SET_CAM_ACTIVE(
                    missionSpectatorCamera_,
                    FALSE);
                CAM::DESTROY_CAM(
                    missionSpectatorCamera_,
                    TRUE);
            }
            missionSpectatorCamera_ = 0;
        }
        for (const auto& [entityId, entry] : worldProxyEntries_) {
            (void)entry;
            const auto proxy =
                worldEntityReplicas_.FindLocal(entityId);
            if (!proxy.has_value() ||
                ENTITY::DOES_ENTITY_EXIST(*proxy) == FALSE) {
                continue;
            }
            ENTITY::FREEZE_ENTITY_POSITION(*proxy, FALSE);
            ENTITY::SET_ENTITY_COLLISION(*proxy, TRUE, TRUE);
            ENTITY::SET_ENTITY_VISIBLE(*proxy, TRUE);
            ENTITY::RESET_ENTITY_ALPHA(*proxy);
        }
        RestoreHiddenAmbientPeds();
        const auto restorePed =
            missionSpectatorPed_ != 0 &&
                    ENTITY::DOES_ENTITY_EXIST(
                        missionSpectatorPed_) != FALSE
                ? static_cast<Ped>(missionSpectatorPed_)
                : localPed;
        const bool usePreparedResume =
            missionResumePrepared_ && !missionResumeFallbackUsed_;
        const auto restorePosition = usePreparedResume
                                         ? missionResumePosition_
                                         : missionSpectatorSavedPosition_;
        const auto restoreHeading = usePreparedResume
                                        ? missionResumeHeading_
                                        : missionSpectatorSavedHeading_;
        if (missionSpectatorMount_ != 0 &&
            ENTITY::DOES_ENTITY_EXIST(missionSpectatorMount_) != FALSE) {
            const auto mountPosition = usePreparedResume
                                           ? missionResumeMountPosition_
                                           : missionSpectatorSavedPosition_;
            ENTITY::SET_ENTITY_COORDS_NO_OFFSET(
                missionSpectatorMount_,
                mountPosition.x,
                mountPosition.y,
                mountPosition.z,
                FALSE,
                FALSE,
                FALSE);
            ENTITY::SET_ENTITY_HEADING(
                missionSpectatorMount_,
                restoreHeading);
            ENTITY::FREEZE_ENTITY_POSITION(
                missionSpectatorMount_,
                FALSE);
            ENTITY::SET_ENTITY_INVINCIBLE(
                missionSpectatorMount_,
                FALSE);
            ENTITY::SET_ENTITY_COLLISION(
                missionSpectatorMount_,
                TRUE,
                TRUE);
            ENTITY::SET_ENTITY_VISIBLE(
                missionSpectatorMount_,
                missionSpectatorMountWasVisible_ ? TRUE : FALSE);
            if (missionSpectatorMountWasVisible_) {
                ENTITY::RESET_ENTITY_ALPHA(missionSpectatorMount_);
            }
        }
        if (restorePed != 0 &&
            ENTITY::DOES_ENTITY_EXIST(restorePed) != FALSE) {
            ENTITY::SET_ENTITY_COORDS_NO_OFFSET(
                restorePed,
                restorePosition.x,
                restorePosition.y,
                restorePosition.z,
                FALSE,
                FALSE,
                FALSE);
            ENTITY::SET_ENTITY_HEADING(
                restorePed,
                restoreHeading);
            ENTITY::FREEZE_ENTITY_POSITION(restorePed, FALSE);
            ENTITY::SET_ENTITY_INVINCIBLE(restorePed, FALSE);
            ENTITY::SET_ENTITY_COLLISION(restorePed, TRUE, TRUE);
            ENTITY::SET_ENTITY_VISIBLE(
                restorePed,
                missionSpectatorWasVisible_ ? TRUE : FALSE);
            if (missionSpectatorWasVisible_) {
                ENTITY::RESET_ENTITY_ALPHA(restorePed);
            }
        }
        PLAYER::SET_PLAYER_CONTROL(
            PLAYER::PLAYER_ID(),
            TRUE,
            0,
            FALSE);
        missionSpectatorActive_ = false;
        missionProxyCastFallbackActive_ = false;
        missionSpectatorStartedAtMs_ = 0U;
        missionSpectatorPed_ = 0;
        missionSpectatorMount_ = 0;
        missionSpectatorWasVisible_ = true;
        missionSpectatorMountWasVisible_ = true;
        missionReplicatedCameraActive_ = false;
        missionReplicatedCameraGeneration_ = 0U;
        missionReplicatedCameraRevision_ = 0U;
        missionReplicatedCameraUpdatedMs_ = 0U;
        missionReplicatedCameraRenderAssertedMs_ = 0U;
        if (missionResumeFallbackUsed_ || !missionResumePrepared_) {
            missionDeferredResumePending_ =
                missionResumePreparing_ && IsFinite(missionResumePosition_);
            missionDeferredResumeRetryMs_ =
                TickMilliseconds() + 1'000U;
        } else {
            missionDeferredResumePending_ = false;
            missionResumePreparing_ = false;
            missionResumePrepared_ = false;
            missionResumeFallbackUsed_ = false;
        }
        if (CAM::IS_SCREEN_FADED_IN() == FALSE &&
            CAM::IS_SCREEN_FADING_IN() == FALSE) {
            CAM::DO_SCREEN_FADE_IN(350);
        }
        Log(
            usePreparedResume
                ? "[MISSION_SPECTATOR] guest restored at prepared host resume anchor"
                : "[MISSION_SPECTATOR] fallback restored the saved pre-scene position");
#else
        (void)active;
#endif
    } catch (...) {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        STREAMING::CLEAR_FOCUS();
        STREAMING::CLEAR_HD_AREA();
        missionStreamingFocusActive_ = false;
#endif
        Log(
            "[ERROR][MISSION_SPECTATOR] exception while applying reversible spectator state");
    }
}

void ScriptHookSdkFacade::MaintainMissionResumeBarrier(
    const bool active) noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        const auto localPed = PLAYER::PLAYER_PED_ID();
        if (active) {
            if (localPed == 0 ||
                ENTITY::DOES_ENTITY_EXIST(localPed) == FALSE) {
                return;
            }
            if (!missionResumeBarrierActive_) {
                missionResumeBarrierActive_ = true;
                missionResumeBarrierPed_ =
                    static_cast<LocalEntityHandle>(localPed);
                Log(
                    "[MISSION_RESUME_BARRIER] host control held until the guest confirms streamed resume state");
            }
            if (CAM::IS_SCREEN_FADED_OUT() == FALSE &&
                CAM::IS_SCREEN_FADING_OUT() == FALSE) {
                CAM::DO_SCREEN_FADE_OUT(150);
            }
            PLAYER::SET_PLAYER_CONTROL(
                PLAYER::PLAYER_ID(),
                FALSE,
                0,
                FALSE);
            ENTITY::FREEZE_ENTITY_POSITION(localPed, TRUE);
            return;
        }

        if (!missionResumeBarrierActive_) {
            return;
        }
        const auto restorePed =
            missionResumeBarrierPed_ != 0 &&
                    ENTITY::DOES_ENTITY_EXIST(
                        missionResumeBarrierPed_) != FALSE
                ? static_cast<Ped>(missionResumeBarrierPed_)
                : localPed;
        if (restorePed != 0 &&
            ENTITY::DOES_ENTITY_EXIST(restorePed) != FALSE) {
            ENTITY::FREEZE_ENTITY_POSITION(restorePed, FALSE);
        }
        PLAYER::SET_PLAYER_CONTROL(
            PLAYER::PLAYER_ID(),
            TRUE,
            0,
            FALSE);
        if (CAM::IS_SCREEN_FADED_IN() == FALSE &&
            CAM::IS_SCREEN_FADING_IN() == FALSE) {
            CAM::DO_SCREEN_FADE_IN(250);
        }
        missionResumeBarrierActive_ = false;
        missionResumeBarrierPed_ = 0;
        Log(
            "[MISSION_RESUME_BARRIER] both endpoints ready; host control released");
#else
        (void)active;
#endif
    } catch (...) {
        Log(
            "[ERROR][MISSION_RESUME_BARRIER] failed to maintain host resume barrier");
    }
}

void ScriptHookSdkFacade::MaintainReplicatedMissionCamera(
    const bool spectatorActive,
    const std::optional<MissionCameraStatePayload>& state) noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        constexpr auto kActive =
            static_cast<std::uint32_t>(
                MissionCameraStateFlag::Active);
        const bool replicatedStateActive =
            spectatorActive && state.has_value() &&
            (state->flags & kActive) != 0U;
        const auto remoteHost = replicas_.FindLocal(remotePlayerId_);
        if (!replicatedStateActive) {
            if (missionNativeAnimSceneActive_) {
                missionReplicatedCameraActive_ = false;
                missionReplicatedCameraGeneration_ = 0U;
                missionReplicatedCameraRevision_ = 0U;
                missionReplicatedCameraUpdatedMs_ = 0U;
                missionReplicatedCameraRenderAssertedMs_ = 0U;
                if (missionSpectatorCamera_ != 0 &&
                    CAM::DOES_CAM_EXIST(
                        missionSpectatorCamera_) != FALSE) {
                    CAM::SET_CAM_ACTIVE(
                        missionSpectatorCamera_,
                        FALSE);
                }
                CAM::RENDER_SCRIPT_CAMS(
                    FALSE,
                    FALSE,
                    0,
                    TRUE,
                    FALSE,
                    0);
                return;
            }
            if (!missionReplicatedCameraActive_) {
                return;
            }
            const auto now = TickMilliseconds();
            const bool shortNetworkGap =
                spectatorActive && !state.has_value() &&
                missionReplicatedCameraUpdatedMs_ != 0U &&
                now >= missionReplicatedCameraUpdatedMs_ &&
                now - missionReplicatedCameraUpdatedMs_ <=
                    kMissionCameraGapHoldMilliseconds;
            if (shortNetworkGap) {
                // Hold the latest cinematic frame through a short UDP gap.
                // Reattaching to the player after one missed snapshot caused
                // the visible host-follow camera in otherwise healthy scenes.
                return;
            }
            missionReplicatedCameraActive_ = false;
            missionReplicatedCameraGeneration_ = 0U;
            missionReplicatedCameraRevision_ = 0U;
            missionReplicatedCameraUpdatedMs_ = 0U;
            missionReplicatedCameraRenderAssertedMs_ = 0U;
            if (missionSpectatorCamera_ != 0 &&
                CAM::DOES_CAM_EXIST(missionSpectatorCamera_) != FALSE &&
                remoteHost.has_value() &&
                ENTITY::DOES_ENTITY_EXIST(*remoteHost) != FALSE) {
                CAM::ATTACH_CAM_TO_ENTITY(
                    missionSpectatorCamera_,
                    *remoteHost,
                    0.0F,
                    -4.0F,
                    2.0F,
                    TRUE);
                CAM::POINT_CAM_AT_ENTITY(
                    missionSpectatorCamera_,
                    *remoteHost,
                    0.0F,
                    0.0F,
                    0.8F,
                    TRUE);
            }
            Log(
                "[MISSION_CAMERA][FALLBACK] replicated camera unavailable; spectator follows the host proxy");
            return;
        }

        if (missionSpectatorCamera_ == 0 ||
            CAM::DOES_CAM_EXIST(missionSpectatorCamera_) == FALSE) {
            char cameraName[] = "DEFAULT_SCRIPTED_CAMERA";
            missionSpectatorCamera_ = CAM::CREATE_CAM_WITH_PARAMS(
                cameraName,
                state->position.x,
                state->position.y,
                state->position.z,
                state->rotation.x,
                state->rotation.y,
                state->rotation.z,
                state->fieldOfView,
                TRUE,
                2);
            if (missionSpectatorCamera_ == 0 ||
                CAM::DOES_CAM_EXIST(missionSpectatorCamera_) == FALSE) {
                return;
            }
            CAM::SET_CAM_ACTIVE(missionSpectatorCamera_, TRUE);
            CAM::RENDER_SCRIPT_CAMS(
                TRUE,
                FALSE,
                0,
                TRUE,
                FALSE,
                0);
        }

        const bool firstReplicatedFrame =
            !missionReplicatedCameraActive_ ||
            state->cinematicGeneration !=
                missionReplicatedCameraGeneration_;
        if (firstReplicatedFrame) {
            CAM::DETACH_CAM(missionSpectatorCamera_);
            CAM::STOP_CAM_POINTING(missionSpectatorCamera_);
            missionReplicatedCameraPosition_ = state->position;
            missionReplicatedCameraRotation_ = state->rotation;
            missionReplicatedCameraFieldOfView_ = state->fieldOfView;
            missionReplicatedCameraActive_ = true;
            missionReplicatedCameraGeneration_ =
                state->cinematicGeneration;
            missionReplicatedCameraRenderAssertedMs_ = 0U;
            constexpr auto kRenderingScript =
                static_cast<std::uint32_t>(
                    MissionCameraStateFlag::SourceRenderingScriptCamera);
            constexpr auto kCinematicGameplay =
                static_cast<std::uint32_t>(
                    MissionCameraStateFlag::SourceCinematicGameplayCamera);
            Log(
                std::string{
                    "[MISSION_CAMERA][RX] host-rendered cutscene camera activated on guest; source="} +
                ((state->flags & kRenderingScript) != 0U
                     ? "rendering-script-camera"
                     : (state->flags & kCinematicGameplay) != 0U
                           ? "cinematic-gameplay-camera"
                           : "gameplay-camera-fallback"));
        }

        if (firstReplicatedFrame ||
            state->revision != missionReplicatedCameraRevision_) {
            missionReplicatedCameraRevision_ = state->revision;
            missionReplicatedCameraTargetPosition_ = state->position;
            missionReplicatedCameraTargetRotation_ = state->rotation;
            missionReplicatedCameraTargetFieldOfView_ =
                state->fieldOfView;
        }

        const auto now = TickMilliseconds();
        missionReplicatedCameraUpdatedMs_ = now;
        // Cinematic cuts are authored discontinuities. Interpolating them as
        // ordinary gameplay motion changes framing and makes the guest trail
        // the host. Apply every authoritative sample exactly; the host stream
        // already runs at 30 Hz.
        missionReplicatedCameraPosition_ =
            missionReplicatedCameraTargetPosition_;
        missionReplicatedCameraRotation_ =
            missionReplicatedCameraTargetRotation_;
        missionReplicatedCameraFieldOfView_ =
            missionReplicatedCameraTargetFieldOfView_;
        const bool renderAssertDue =
            firstReplicatedFrame ||
            missionReplicatedCameraRenderAssertedMs_ == 0U ||
            now < missionReplicatedCameraRenderAssertedMs_ ||
            now - missionReplicatedCameraRenderAssertedMs_ >=
                kMissionCameraRenderAssertMilliseconds;
        if (renderAssertDue) {
            CAM::DETACH_CAM(missionSpectatorCamera_);
            CAM::STOP_CAM_POINTING(missionSpectatorCamera_);
            CAM::SET_CAM_ACTIVE(missionSpectatorCamera_, TRUE);
            CAM::RENDER_SCRIPT_CAMS(
                TRUE,
                FALSE,
                0,
                TRUE,
                FALSE,
                0);
            missionReplicatedCameraRenderAssertedMs_ = now;
        }
        CAM::SET_CAM_COORD(
            missionSpectatorCamera_,
            missionReplicatedCameraPosition_.x,
            missionReplicatedCameraPosition_.y,
            missionReplicatedCameraPosition_.z);
        CAM::SET_CAM_ROT(
            missionSpectatorCamera_,
            missionReplicatedCameraRotation_.x,
            missionReplicatedCameraRotation_.y,
            missionReplicatedCameraRotation_.z,
            2);
        CAM::SET_CAM_FOV(
            missionSpectatorCamera_,
            missionReplicatedCameraFieldOfView_);
        STREAMING::_SET_FOCUS_AREA(
            missionReplicatedCameraPosition_.x,
            missionReplicatedCameraPosition_.y,
            missionReplicatedCameraPosition_.z,
            0.0F,
            0.0F,
            0.0F);
        STREAMING::SET_HD_AREA(
            missionReplicatedCameraPosition_.x,
            missionReplicatedCameraPosition_.y,
            missionReplicatedCameraPosition_.z,
            kMissionCinematicStreamingRadiusMeters);
        missionStreamingFocus_ = missionReplicatedCameraPosition_;
        missionStreamingFocusActive_ = true;

        constexpr auto kFadedOut = static_cast<std::uint32_t>(
            MissionCameraStateFlag::ScreenFadedOut);
        constexpr auto kFadingOut = static_cast<std::uint32_t>(
            MissionCameraStateFlag::ScreenFadingOut);
        constexpr auto kFadingIn = static_cast<std::uint32_t>(
            MissionCameraStateFlag::ScreenFadingIn);
        if ((state->flags & (kFadedOut | kFadingOut)) != 0U) {
            if (CAM::IS_SCREEN_FADED_OUT() == FALSE &&
                CAM::IS_SCREEN_FADING_OUT() == FALSE) {
                CAM::DO_SCREEN_FADE_OUT(150);
            }
        } else if ((state->flags & kFadingIn) != 0U) {
            if (CAM::IS_SCREEN_FADED_IN() == FALSE &&
                CAM::IS_SCREEN_FADING_IN() == FALSE) {
                CAM::DO_SCREEN_FADE_IN(150);
            }
        } else if (!missionResumePreparing_ &&
                   CAM::IS_SCREEN_FADED_OUT() != FALSE &&
                   CAM::IS_SCREEN_FADING_IN() == FALSE) {
            CAM::DO_SCREEN_FADE_IN(150);
        }
#else
        (void)spectatorActive;
        (void)state;
#endif
    } catch (...) {
        Log(
            "[ERROR][MISSION_CAMERA][RX] failed to apply the replicated mission camera");
    }
}

ReplicatedAnimScenePrepareResult
ScriptHookSdkFacade::PrepareReplicatedAnimSceneDefinition(
    const AnimSceneDefinitionPayload& definition,
    const NetEntityId localEntityId) noexcept {
    std::uint16_t requiredRoles{};
    for (const auto& role : definition.roles) {
        if ((role.flags & static_cast<std::uint16_t>(
                              AnimSceneRoleFlag::Required)) != 0U) {
            ++requiredRoles;
        }
    }
    if (!animSceneHybridNativeCreationEnabled_) {
        if (!animSceneHybridPreparationWarningLogged_) {
            animSceneHybridPreparationWarningLogged_ = true;
            Log(
                "[ANIMSCENE_HYBRID][NATIVE_CREATE][DISABLED] runtime capture/detour has not passed the exact-build live handler probe; rejecting exact creation fail-closed");
        }
        (void)localEntityId;
        return {
            ReplicatedAnimScenePrepareStatus::Unsupported,
            0U,
            requiredRoles,
            false,
            false,
            ReplicatedAnimScenePrepareStage::Failed};
    }

    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        const bool sameDefinition =
            ownedHybridAnimSceneHandle_ > 0 &&
            ownedHybridAnimSceneDefinitionRevision_ ==
                definition.definitionRevision &&
            ownedHybridAnimSceneFingerprintLow_ ==
                definition.fingerprintLow &&
            ownedHybridAnimSceneFingerprintHigh_ ==
                definition.fingerprintHigh;
        if (ownedHybridAnimSceneHandle_ > 0 && !sameDefinition) {
            AbortReplicatedAnimSceneDefinition();
        }
        if (ownedHybridAnimSceneHandle_ == 0) {
            ownedHybridAnimSceneHandle_ = CreateAnimScene(
                definition.resourceName.c_str(),
                definition.sceneFlags,
                definition.playbackList.c_str(),
                definition.createOptionFlags);
            if (ownedHybridAnimSceneHandle_ <= 0 ||
                DoesAnimSceneExist(ownedHybridAnimSceneHandle_) == FALSE) {
                ownedHybridAnimSceneHandle_ = 0;
                return {
                    ReplicatedAnimScenePrepareStatus::NativeFailure,
                    0U,
                    requiredRoles,
                    false,
                    false,
                    ReplicatedAnimScenePrepareStage::Failed};
            }
            ownedHybridAnimSceneDefinitionRevision_ =
                definition.definitionRevision;
            ownedHybridAnimSceneFingerprintLow_ = definition.fingerprintLow;
            ownedHybridAnimSceneFingerprintHigh_ = definition.fingerprintHigh;
            ownedHybridAnimSceneResolvedRoles_ = 0U;
            ownedHybridAnimSceneBoundRoles_.clear();
            ownedHybridAnimSceneBoundEntities_.clear();
            ownedHybridAnimSceneLoadRequested_ = false;
            ownedHybridAnimSceneStarted_ = false;
            Log(
                "[ANIMSCENE_HYBRID][NATIVE_CREATE] bridge-owned handle=" +
                std::to_string(ownedHybridAnimSceneHandle_) +
                ", resource=" + definition.resourceName +
                ", required-roles=" + std::to_string(requiredRoles));
        }

        const auto resolveEntity = [this, localEntityId](
                                       const NetEntityId entityId) {
            if (entityId == localEntityId) {
                const auto localPed = PLAYER::PLAYER_PED_ID();
                return localPed != 0 &&
                               ENTITY::DOES_ENTITY_EXIST(localPed) != FALSE
                           ? static_cast<Entity>(localPed)
                           : static_cast<Entity>(0);
            }
            if (const auto player = replicas_.FindLocal(entityId);
                player.has_value()) {
                return static_cast<Entity>(*player);
            }
            if (const auto mount = remoteMountReplicas_.FindLocal(entityId);
                mount.has_value()) {
                return static_cast<Entity>(*mount);
            }
            if (const auto world = worldEntityReplicas_.FindLocal(entityId);
                world.has_value()) {
                return static_cast<Entity>(*world);
            }
            return static_cast<Entity>(0);
        };

        struct BindingPassResult final {
            std::uint16_t resolvedRequired{};
            bool missingRequired{};
            bool invalidRequired{};
            bool entityMismatch{};
            NetEntityId firstPendingEntityId{};
            std::string firstPendingRoleName;
        };
        const auto bindAvailableRoles = [&]() {
            BindingPassResult result;
            for (const auto& role : definition.roles) {
                constexpr auto kRequired = static_cast<std::uint16_t>(
                    AnimSceneRoleFlag::Required);
                const bool required = (role.flags & kRequired) != 0U;
                if (!role.entityId.IsValid()) {
                    if (required) {
                        result.invalidRequired = true;
                        if (result.firstPendingRoleName.empty()) {
                            result.firstPendingRoleName = role.roleName;
                        }
                    }
                    continue;
                }
                const auto entity = resolveEntity(role.entityId);
                if (entity == 0 ||
                    ENTITY::DOES_ENTITY_EXIST(entity) == FALSE) {
                    if (required) {
                        result.missingRequired = true;
                        if (result.firstPendingRoleName.empty()) {
                            result.firstPendingRoleName = role.roleName;
                            result.firstPendingEntityId = role.entityId;
                        }
                    }
                    continue;
                }
                if (role.modelHash != 0U &&
                    static_cast<std::uint32_t>(
                        ENTITY::GET_ENTITY_MODEL(entity)) != role.modelHash) {
                    result.entityMismatch = true;
                    result.firstPendingEntityId = role.entityId;
                    result.firstPendingRoleName = role.roleName;
                    break;
                }
                const bool alreadyBound =
                    std::find(
                        ownedHybridAnimSceneBoundRoles_.begin(),
                        ownedHybridAnimSceneBoundRoles_.end(),
                        role.roleName) !=
                    ownedHybridAnimSceneBoundRoles_.end();
                if (!alreadyBound) {
                    SetAnimSceneEntity(
                        ownedHybridAnimSceneHandle_,
                        role.roleName.c_str(),
                        entity,
                        role.bindingFlags);
                    ownedHybridAnimSceneBoundRoles_.push_back(role.roleName);
                    const auto localHandle =
                        static_cast<LocalEntityHandle>(entity);
                    if (std::find(
                            ownedHybridAnimSceneBoundEntities_.begin(),
                            ownedHybridAnimSceneBoundEntities_.end(),
                            localHandle) ==
                        ownedHybridAnimSceneBoundEntities_.end()) {
                        ownedHybridAnimSceneBoundEntities_.push_back(
                            localHandle);
                    }
                }
                if (required) {
                    ++result.resolvedRequired;
                }
            }
            return result;
        };

        // RDR2 must receive the complete required cast before LOAD. The V31.9
        // ODR1_INT trace proved that loading with only the two player roles
        // (2/22) never recovered after the remaining world spawns arrived:
        // IS_ANIM_SCENE_LOADED stayed false until timeout. Re-run this binding
        // pass on every prepare poll and keep the scene un-loaded until every
        // required actor exists locally and matches the captured model.
        auto bindings = bindAvailableRoles();
        ownedHybridAnimSceneResolvedRoles_ = bindings.resolvedRequired;
        if (bindings.entityMismatch) {
            return {
                ReplicatedAnimScenePrepareStatus::EntityMismatch,
                bindings.resolvedRequired,
                requiredRoles,
                false,
                sameDefinition,
                ReplicatedAnimScenePrepareStage::Failed,
                bindings.firstPendingEntityId,
                std::move(bindings.firstPendingRoleName)};
        }
        if (bindings.invalidRequired) {
            return {
                ReplicatedAnimScenePrepareStatus::MissingBinding,
                bindings.resolvedRequired,
                requiredRoles,
                false,
                sameDefinition,
                ReplicatedAnimScenePrepareStage::Failed,
                bindings.firstPendingEntityId,
                std::move(bindings.firstPendingRoleName)};
        }
        if (bindings.missingRequired) {
            return {
                ReplicatedAnimScenePrepareStatus::Pending,
                bindings.resolvedRequired,
                requiredRoles,
                false,
                sameDefinition,
                ReplicatedAnimScenePrepareStage::WaitingForBindings,
                bindings.firstPendingEntityId,
                std::move(bindings.firstPendingRoleName)};
        }

        // Use the captured dependency order exactly:
        // CREATE -> SET_ENTITY(all required roles) -> LOAD -> START.
        if (!ownedHybridAnimSceneLoadRequested_) {
            // CREATE already received the captured final playback list. A
            // second SET_PLAYBACK_LIST here could reset internal streaming.
            LoadAnimScene(ownedHybridAnimSceneHandle_);
            ownedHybridAnimSceneLoadRequested_ = true;
            Log(
                "[ANIMSCENE_HYBRID][NATIVE_LOAD] handle=" +
                std::to_string(ownedHybridAnimSceneHandle_) +
                ", prebound=" +
                std::to_string(ownedHybridAnimSceneBoundRoles_.size()) +
                "/" + std::to_string(definition.roles.size()) +
                ", required=" +
                std::to_string(bindings.resolvedRequired) + "/" +
                std::to_string(requiredRoles));
        }
        const bool loaded =
            IsAnimSceneLoaded(ownedHybridAnimSceneHandle_) != FALSE;

        // Do not START or declare readiness until RDR2 confirms the resource.
        // Retry the binding pass while streaming so optional late actors can
        // still join without ever letting a missing required actor through.
        if (!loaded) {
            return {
                ReplicatedAnimScenePrepareStatus::Pending,
                bindings.resolvedRequired,
                requiredRoles,
                false,
                sameDefinition,
                ReplicatedAnimScenePrepareStage::WaitingForResource};
        }

        bindings = bindAvailableRoles();
        ownedHybridAnimSceneResolvedRoles_ = bindings.resolvedRequired;
        if (bindings.entityMismatch) {
            return {
                ReplicatedAnimScenePrepareStatus::EntityMismatch,
                bindings.resolvedRequired,
                requiredRoles,
                true,
                sameDefinition,
                ReplicatedAnimScenePrepareStage::Failed,
                bindings.firstPendingEntityId,
                std::move(bindings.firstPendingRoleName)};
        }
        if (bindings.invalidRequired) {
            return {
                ReplicatedAnimScenePrepareStatus::MissingBinding,
                bindings.resolvedRequired,
                requiredRoles,
                true,
                sameDefinition,
                ReplicatedAnimScenePrepareStage::Failed,
                bindings.firstPendingEntityId,
                std::move(bindings.firstPendingRoleName)};
        }
        if (bindings.missingRequired) {
            return {
                ReplicatedAnimScenePrepareStatus::Pending,
                bindings.resolvedRequired,
                requiredRoles,
                true,
                sameDefinition,
                ReplicatedAnimScenePrepareStage::WaitingForBindings,
                bindings.firstPendingEntityId,
                std::move(bindings.firstPendingRoleName)};
        }
        return {
            ReplicatedAnimScenePrepareStatus::Ready,
            bindings.resolvedRequired,
            requiredRoles,
            true,
            sameDefinition,
            ReplicatedAnimScenePrepareStage::Ready};
#else
        (void)definition;
        (void)localEntityId;
#endif
    } catch (...) {
        Log(
            "[ANIMSCENE_HYBRID][NATIVE_CREATE][FAILED] exception while creating or binding bridge-owned guest scene");
    }
    AbortReplicatedAnimSceneDefinition();
    return {
        ReplicatedAnimScenePrepareStatus::NativeFailure,
        0U,
        requiredRoles,
        false,
        false,
        ReplicatedAnimScenePrepareStage::Failed};
}

bool ScriptHookSdkFacade::MaintainHostAnimSceneStartBarrier(
    const bool active) noexcept {
    // The sampled handle belongs to RDR2's Story VM. Pausing it while the
    // guest streams a speculative replica also pauses the host-side actor
    // assignment and audio timeline. V31.5 proved that this can make even the
    // authoritative host scene render without its cast. Keep only a logical
    // 2PC marker; the game-owned scene is observation-only and never mutated.
    hostAnimSceneStartBarrierHandle_ = 0;
    hostAnimSceneStartBarrierActive_ = active;
    return true;
}

bool ScriptHookSdkFacade::CommitReplicatedAnimSceneDefinition(
    const AnimSceneControlPayload& commit) noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        if (!animSceneHybridNativeCreationEnabled_ ||
            ownedHybridAnimSceneHandle_ <= 0 ||
            ownedHybridAnimSceneDefinitionRevision_ !=
                commit.definitionRevision ||
            ownedHybridAnimSceneFingerprintLow_ != commit.fingerprintLow ||
            ownedHybridAnimSceneFingerprintHigh_ != commit.fingerprintHigh ||
            !ownedHybridAnimSceneLoadRequested_ ||
            IsAnimSceneLoaded(ownedHybridAnimSceneHandle_) == FALSE) {
            return false;
        }
        // CREATE/LOAD happens after the host Story VM has already started its
        // scene. Begin a late guest scene at bounded fast-forward when the
        // committed phase is non-zero; the live phase controller below then
        // converges it to the host instead of replaying the scene from frame 0.
        const auto initialRate =
            commit.startPhase > 0.001F
                ? 4.0F
                : std::clamp(commit.rate, 0.0F, 4.0F);
        SetAnimSceneRate(
            ownedHybridAnimSceneHandle_,
            initialRate);
        StartAnimScene(ownedHybridAnimSceneHandle_);
        SetAnimSceneRate(
            ownedHybridAnimSceneHandle_,
            initialRate);
        ownedHybridAnimSceneStarted_ = true;
        guestAnimSceneHandle_ = ownedHybridAnimSceneHandle_;
        guestAnimSceneGeneration_ = commit.cinematicGeneration;
        guestAnimSceneRestartAttempted_ = false;
        Log(
            "[ANIMSCENE_HYBRID][NATIVE_START] bridge-owned exact scene committed; handle=" +
            std::to_string(ownedHybridAnimSceneHandle_) +
            ", phase-at-commit=" + std::to_string(commit.startPhase));
        return true;
#else
        (void)commit;
#endif
    } catch (...) {
        Log(
            "[ANIMSCENE_HYBRID][NATIVE_START][FAILED] native start rejected or raised an exception");
    }
    return false;
}

void ScriptHookSdkFacade::AbortReplicatedAnimSceneDefinition() noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        const auto ownedHandle = ownedHybridAnimSceneHandle_;
        if (ownedHandle > 0 && DoesAnimSceneExist(ownedHandle) != FALSE) {
            DeleteAnimScene(ownedHandle);
            Log(
                "[ANIMSCENE_HYBRID][NATIVE_DELETE] released bridge-owned guest scene handle=" +
                std::to_string(ownedHandle));
        }
        if (guestAnimSceneHandle_ == ownedHandle) {
            guestAnimSceneHandle_ = 0;
            missionNativeAnimSceneActive_ = false;
        }
#endif
    } catch (...) {
        Log(
            "[ANIMSCENE_HYBRID][NATIVE_DELETE][FAILED] bridge-owned handle cleanup failed safely");
    }
    ownedHybridAnimSceneHandle_ = 0;
    ownedHybridAnimSceneDefinitionRevision_ = 0U;
    ownedHybridAnimSceneFingerprintLow_ = 0U;
    ownedHybridAnimSceneFingerprintHigh_ = 0U;
    ownedHybridAnimSceneResolvedRoles_ = 0U;
    ownedHybridAnimSceneBoundRoles_.clear();
    ownedHybridAnimSceneBoundEntities_.clear();
    ownedHybridAnimSceneLoadRequested_ = false;
    ownedHybridAnimSceneStarted_ = false;
}

bool ScriptHookSdkFacade::MaintainReplicatedAnimScene(
    const bool spectatorActive,
    const std::optional<AnimSceneReplicaStatePayload>& state) noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        constexpr auto kActive = static_cast<std::uint32_t>(
            AnimSceneReplicaStateFlag::Active);
        constexpr auto kRunning = static_cast<std::uint32_t>(
            AnimSceneReplicaStateFlag::Running);
        constexpr auto kLoaded = static_cast<std::uint32_t>(
            AnimSceneReplicaStateFlag::Loaded);
        constexpr auto kCamera = static_cast<std::uint32_t>(
            AnimSceneReplicaStateFlag::CameraActive);
        constexpr auto kOrigin = static_cast<std::uint32_t>(
            AnimSceneReplicaStateFlag::OriginValid);
        const auto clear = [this]() {
            const bool wasActive = missionNativeAnimSceneActive_;
            if (guestAnimSceneHandle_ > 0 &&
                DoesAnimSceneExist(guestAnimSceneHandle_) != FALSE) {
                if (guestAnimScenePausedByBridge_) {
                    SetAnimScenePaused(guestAnimSceneHandle_, false);
                }
                SetAnimSceneRate(guestAnimSceneHandle_, 1.0F);
            }
            guestAnimSceneHandle_ = 0;
            guestAnimSceneProbeCursor_ = 1;
            guestAnimSceneProbeAttempts_ = 0U;
            guestAnimSceneDictionaryHash_ = 0U;
            guestAnimSceneGeneration_ = 0U;
            guestAnimSceneLastProbeLogMs_ = 0U;
            guestAnimSceneLastCorrectionLogMs_ = 0U;
            guestAnimSceneLastFailureLogMs_ = 0U;
            guestAnimScenePausedByBridge_ = false;
            guestAnimSceneRestartAttempted_ = false;
            missionNativeAnimSceneActive_ = false;
            missionNativeAnimSceneOrigin_ = {};
            if (wasActive) {
                Log(
                    "[ANIMSCENE_REPLICA][TEARDOWN] released local scene rate/pause without deleting or taking ownership");
            }
        };

        const bool requested =
            spectatorActive && state.has_value() &&
            state->hostEntityId.IsValid() &&
            state->missionEpoch != 0U &&
            state->cinematicGeneration != 0U &&
            state->dictionaryHash != 0U &&
            (state->flags & kActive) != 0U &&
            std::isfinite(state->phase) &&
            state->phase >= 0.0F && state->phase <= 1.05F &&
            std::isfinite(state->durationSeconds) &&
            state->durationSeconds > 0.0F &&
            std::isfinite(state->rate);
        if (!requested) {
            clear();
            return false;
        }

        const bool signatureChanged =
            guestAnimSceneDictionaryHash_ != 0U &&
            (guestAnimSceneDictionaryHash_ != state->dictionaryHash ||
             guestAnimSceneGeneration_ !=
                 state->cinematicGeneration);
        if (signatureChanged) {
            clear();
        }
        guestAnimSceneDictionaryHash_ = state->dictionaryHash;
        guestAnimSceneGeneration_ = state->cinematicGeneration;

        const auto signatureMatches = [&state](const int scene) {
            if (scene <= 0 || DoesAnimSceneExist(scene) == FALSE) {
                return false;
            }
            const auto dictionary = static_cast<std::uint32_t>(
                GetAnimSceneDictionary(scene));
            const auto duration = GetAnimSceneDuration(scene);
            return dictionary == state->dictionaryHash &&
                   std::isfinite(duration) &&
                   std::abs(duration - state->durationSeconds) <=
                       kAnimSceneDurationToleranceSeconds;
        };
        if (!signatureMatches(guestAnimSceneHandle_)) {
            if (guestAnimSceneHandle_ > 0) {
                guestAnimSceneProbeCursor_ = 1;
                guestAnimSceneProbeAttempts_ = 0U;
                guestAnimSceneLastProbeLogMs_ = 0U;
            }
            guestAnimSceneHandle_ = 0;
            guestAnimSceneRestartAttempted_ = false;
        }
        if (guestAnimSceneHandle_ == 0) {
            for (int attempt = 0;
                 attempt < kAnimSceneProbeBatchSize;
                 ++attempt) {
                if (guestAnimSceneProbeCursor_ <= 0 ||
                    guestAnimSceneProbeCursor_ >
                        kAnimSceneMaximumProbeHandle) {
                    guestAnimSceneProbeCursor_ = 1;
                }
                const auto candidate = guestAnimSceneProbeCursor_++;
                if (guestAnimSceneProbeAttempts_ <
                    static_cast<std::uint32_t>(
                        kAnimSceneMaximumProbeHandle)) {
                    ++guestAnimSceneProbeAttempts_;
                }
                // A dictionary can have a stale preloaded duplicate. Bind only
                // to the instance that is actually running/loaded and owns an
                // authored camera; after binding, retain that handle across a
                // short engine stop so it can be restarted once below.
                if (signatureMatches(candidate) &&
                    (IsAnimSceneLoaded(candidate) != FALSE ||
                     IsAnimSceneRunning(candidate) != FALSE) &&
                    GetAnimSceneActiveCameraCount(candidate) > 0) {
                    guestAnimSceneHandle_ = candidate;
                    guestAnimSceneProbeAttempts_ = 0U;
                    guestAnimSceneLastProbeLogMs_ = 0U;
                    guestAnimSceneRestartAttempted_ = false;
                    break;
                }
            }
        }
        if (guestAnimSceneHandle_ == 0) {
            const auto now = TickMilliseconds();
            const bool completeProbe =
                guestAnimSceneProbeAttempts_ >=
                static_cast<std::uint32_t>(
                    kAnimSceneMaximumProbeHandle);
            if (completeProbe &&
                (guestAnimSceneLastProbeLogMs_ == 0U ||
                now < guestAnimSceneLastProbeLogMs_ ||
                now - guestAnimSceneLastProbeLogMs_ >=
                    kAnimSceneProbeLogMilliseconds)) {
                guestAnimSceneLastProbeLogMs_ = now;
                Log(
                    "[ANIMSCENE_REPLICA][SAFE_FALLBACK] full local handle sweep found no matching dictionary/duration scene; host camera plus non-colliding kinematic proxy cast remain authoritative");
            }
            missionNativeAnimSceneActive_ = false;
            return false;
        }

        const bool firstAttach = !missionNativeAnimSceneActive_;
        if (firstAttach && (state->flags & kOrigin) != 0U &&
            IsFinite(state->originPosition) &&
            IsFinite(state->originRotation)) {
            SetAnimSceneOrigin(
                guestAnimSceneHandle_,
                state->originPosition,
                state->originRotation);
            missionNativeAnimSceneOrigin_ = state->originPosition;
        }
        const bool hostRunning = (state->flags & kRunning) != 0U;
        const bool hostLoaded = (state->flags & kLoaded) != 0U;
        auto localRunning =
            IsAnimSceneRunning(guestAnimSceneHandle_) != FALSE;
        const bool localLoaded =
            IsAnimSceneLoaded(guestAnimSceneHandle_) != FALSE;
        if (hostRunning && hostLoaded && localLoaded && !localRunning &&
            !guestAnimSceneRestartAttempted_) {
            guestAnimSceneRestartAttempted_ = true;
            StartAnimScene(guestAnimSceneHandle_);
            localRunning =
                IsAnimSceneRunning(guestAnimSceneHandle_) != FALSE;
            Log(
                localRunning
                    ? "[ANIMSCENE_REPLICA][RECOVERED] retained matching local scene restarted once after its Story VM stopped it"
                    : "[WARNING][ANIMSCENE_REPLICA][DETACHED] matching local scene restart was rejected by RDR2; safe camera fallback retained");
        }
        if (!localRunning) {
            missionNativeAnimSceneActive_ = false;
            const auto now = TickMilliseconds();
            if (guestAnimSceneLastFailureLogMs_ == 0U ||
                now < guestAnimSceneLastFailureLogMs_ ||
                now - guestAnimSceneLastFailureLogMs_ >=
                    kAnimSceneProbeLogMilliseconds) {
                guestAnimSceneLastFailureLogMs_ = now;
                Log(
                    "[ANIMSCENE_REPLICA][DETACHED] reason=matching-handle-not-running, local-loaded=" +
                    std::to_string(localLoaded ? 1 : 0) +
                    ", restart-attempted=" +
                    std::to_string(guestAnimSceneRestartAttempted_ ? 1 : 0));
            }
            return false;
        }

        const auto localPhase =
            GetAnimScenePhase(guestAnimSceneHandle_);
        if (!std::isfinite(localPhase)) {
            clear();
            return false;
        }
        const auto phaseError = state->phase - localPhase;
        if (phaseError < -kAnimScenePhasePauseThreshold) {
            SetAnimScenePaused(guestAnimSceneHandle_, true);
            guestAnimScenePausedByBridge_ = true;
        } else {
            if (guestAnimScenePausedByBridge_) {
                SetAnimScenePaused(guestAnimSceneHandle_, false);
                guestAnimScenePausedByBridge_ = false;
            }
            // AnimScene rate is duration-relative. The old fixed phase gain
            // needed tens of seconds to recover a one- or two-second prepare
            // delay in a long Story scene. Convert phase error back to seconds
            // and converge over a bounded 750 ms window instead.
            const auto catchUpRate =
                std::max(0.0F, phaseError) *
                state->durationSeconds /
                kAnimScenePhaseCatchUpWindowSeconds;
            const auto correctedRate = std::clamp(
                state->rate + catchUpRate,
                0.0F,
                4.0F);
            SetAnimSceneRate(
                guestAnimSceneHandle_,
                correctedRate);
        }

        const auto localCameraCount = std::clamp(
            GetAnimSceneActiveCameraCount(guestAnimSceneHandle_),
            0,
            32);
        const bool ownsPresentation =
            (state->flags & kCamera) != 0U &&
            state->activeCameraCount > 0U &&
            localCameraCount > 0;
        missionNativeAnimSceneActive_ = ownsPresentation;
        if (!ownsPresentation) {
            const auto now = TickMilliseconds();
            if (guestAnimSceneLastFailureLogMs_ == 0U ||
                now < guestAnimSceneLastFailureLogMs_ ||
                now - guestAnimSceneLastFailureLogMs_ >=
                    kAnimSceneProbeLogMilliseconds) {
                guestAnimSceneLastFailureLogMs_ = now;
                Log(
                    "[ANIMSCENE_REPLICA][DETACHED] reason=local-authored-camera-unavailable, host-cameras=" +
                    std::to_string(state->activeCameraCount) +
                    ", guest-cameras=" +
                    std::to_string(localCameraCount));
            }
            return false;
        }
        guestAnimSceneLastFailureLogMs_ = 0U;

        if (missionSpectatorCamera_ != 0 &&
            CAM::DOES_CAM_EXIST(missionSpectatorCamera_) != FALSE) {
            CAM::SET_CAM_ACTIVE(missionSpectatorCamera_, FALSE);
        }
        CAM::RENDER_SCRIPT_CAMS(
            FALSE,
            FALSE,
            0,
            TRUE,
            FALSE,
            0);
        if ((state->flags & kOrigin) != 0U) {
            STREAMING::_SET_FOCUS_AREA(
                state->originPosition.x,
                state->originPosition.y,
                state->originPosition.z,
                0.0F,
                0.0F,
                0.0F);
            STREAMING::SET_HD_AREA(
                state->originPosition.x,
                state->originPosition.y,
                state->originPosition.z,
                kMissionCinematicStreamingRadiusMeters);
            missionStreamingFocus_ = state->originPosition;
            missionStreamingFocusActive_ = true;
        }
        if (firstAttach) {
            Log(
                "[ANIMSCENE_REPLICA][ATTACHED] matching local scene now supplies authored camera, cast animation, dialogue, subtitles and audio");
        }
        const auto now = TickMilliseconds();
        if (guestAnimSceneLastCorrectionLogMs_ == 0U ||
            now < guestAnimSceneLastCorrectionLogMs_ ||
            now - guestAnimSceneLastCorrectionLogMs_ >=
                kAnimSceneProbeLogMilliseconds) {
            guestAnimSceneLastCorrectionLogMs_ = now;
            Log(
                "[ANIMSCENE_REPLICA][PHASE] host=" +
                std::to_string(state->phase) +
                ", guest=" + std::to_string(localPhase) +
                ", error=" + std::to_string(phaseError) +
                ", paused=" +
                std::to_string(guestAnimScenePausedByBridge_));
        }
        return true;
#else
        (void)spectatorActive;
        (void)state;
#endif
    } catch (...) {
        guestAnimSceneHandle_ = 0;
        guestAnimSceneProbeCursor_ = 1;
        guestAnimSceneProbeAttempts_ = 0U;
        guestAnimSceneDictionaryHash_ = 0U;
        guestAnimSceneGeneration_ = 0U;
        guestAnimSceneLastProbeLogMs_ = 0U;
        guestAnimSceneLastCorrectionLogMs_ = 0U;
        guestAnimSceneLastFailureLogMs_ = 0U;
        guestAnimScenePausedByBridge_ = false;
        guestAnimSceneRestartAttempted_ = false;
        missionNativeAnimSceneActive_ = false;
        Log(
            "[ERROR][ANIMSCENE_REPLICA][RX] native scene attach failed safely; camera-keyframe fallback retained");
    }
    return false;
}

void ScriptHookSdkFacade::MaintainMissionCompanionPresentation(
    const MissionCompanionPresentation& state) noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        const bool active =
            state.active && IsFinite(state.target) &&
            std::isfinite(state.distanceMeters) &&
            state.distanceMeters >= 0.0F;
        if (!active) {
            if (missionObjectiveBlip_ != 0) {
                auto blip = static_cast<Blip>(missionObjectiveBlip_);
                if (RADAR::DOES_BLIP_EXIST(blip) != FALSE) {
                    RADAR::REMOVE_BLIP(&blip);
                }
                missionObjectiveBlip_ = 0;
            }
            missionObjectiveBlipRetryMs_ = 0U;
            if (missionCompanionPresentationActive_) {
                Log(
                    "[MISSION_OBJECTIVE] companion objective hidden outside active host gameplay");
            }
            missionCompanionPresentationActive_ = false;
            missionCompanionPresentationLive_ = false;
            missionCompanionTarget_ = {};
            return;
        }

        const bool wasActive = missionCompanionPresentationActive_;
        const bool sourceChanged =
            wasActive &&
            missionCompanionPresentationLive_ !=
                state.liveHostPosition;
        missionCompanionPresentationActive_ = true;
        missionCompanionPresentationLive_ =
            state.liveHostPosition;
        if (!wasActive) {
            missionCompanionTarget_ = state.target;
        } else {
            constexpr float kTargetSmoothing = 0.35F;
            missionCompanionTarget_.x +=
                (state.target.x - missionCompanionTarget_.x) *
                kTargetSmoothing;
            missionCompanionTarget_.y +=
                (state.target.y - missionCompanionTarget_.y) *
                kTargetSmoothing;
            missionCompanionTarget_.z +=
                (state.target.z - missionCompanionTarget_.z) *
                kTargetSmoothing;
        }

        const auto now = TickMilliseconds();
        if ((missionObjectiveBlip_ == 0 ||
             RADAR::DOES_BLIP_EXIST(
                 static_cast<Blip>(missionObjectiveBlip_)) == FALSE) &&
            now >= missionObjectiveBlipRetryMs_) {
            const auto remoteHost =
                replicas_.FindLocal(remotePlayerId_);
            if (remoteHost.has_value() &&
                ENTITY::DOES_ENTITY_EXIST(*remoteHost) != FALSE) {
                char objectiveStyle[] = "BLIP_STYLE_OBJECTIVE";
                missionObjectiveBlip_ = static_cast<int>(
                    RADAR::_0x23F74C2FDA6E7C61(
                        GAMEPLAY::GET_HASH_KEY(objectiveStyle),
                        *remoteHost));
                if (missionObjectiveBlip_ != 0) {
                    RADAR::SET_BLIP_SCALE(
                        static_cast<Blip>(missionObjectiveBlip_),
                        0.90F);
                }
            }
            missionObjectiveBlipRetryMs_ = now + 1'000U;
        }

        if (!wasActive || sourceChanged) {
            Log(
                std::string{"[MISSION_OBJECTIVE] companion objective active; anchor-source="} +
                (state.liveHostPosition
                     ? "live-player"
                     : "mission-anchor") +
                ", minimap-blip=" +
                std::to_string(missionObjectiveBlip_ != 0));
        }

        // ScriptHook exposes no verified getter for the current localized
        // objective line owned by another process's Story VM. Show a stable
        // mission-context panel instead of the misleading old "follow host"
        // wording. The yellow world/minimap marker remains the spatial aid;
        // this panel tells the guest that the active task is the shared Story
        // objective, not a generic follow-me command.
        DrawNativeRectangle(
            0.785F,
            0.885F,
            0.390F,
            0.067F,
            10,
            9,
            7,
            198);
        DrawNativeRectangle(
            0.590F,
            0.885F,
            0.004F,
            0.067F,
            244,
            190,
            45,
            238);
        DrawNativeText(
            "ACTIVE HOST MISSION",
            0.604F,
            0.858F,
            0.285F,
            244,
            190,
            45,
            255,
            false);
        DrawNativeText(
            "SHARED STORY OBJECTIVE - complete the current task with the host",
            0.604F,
            0.887F,
            0.225F,
            238,
            230,
            208,
            250,
            false);

        float screenX{};
        float screenY{};
        if (GRAPHICS::GET_SCREEN_COORD_FROM_WORLD_COORD(
                missionCompanionTarget_.x,
                missionCompanionTarget_.y,
                missionCompanionTarget_.z + 1.25F,
                &screenX,
                &screenY) != FALSE) {
            constexpr int kRed = 244;
            constexpr int kGreen = 190;
            constexpr int kBlue = 45;
            DrawNativeRectangle(
                screenX,
                screenY,
                0.030F,
                0.0032F,
                kRed,
                kGreen,
                kBlue,
                238);
            DrawNativeRectangle(
                screenX,
                screenY,
                0.0032F,
                0.050F,
                kRed,
                kGreen,
                kBlue,
                238);
            char markerText[96]{};
            std::snprintf(
                markerText,
                sizeof(markerText),
                "STORY MISSION TARGET  %.0f m",
                static_cast<double>(state.distanceMeters));
            DrawNativeText(
                markerText,
                screenX,
                screenY - 0.033F,
                0.24F,
                kRed,
                kGreen,
                kBlue,
                245,
                true);
        }
#else
        (void)state;
#endif
    } catch (...) {
        Log(
            "[ERROR][MISSION_OBJECTIVE] failed to maintain companion marker");
    }
}

RuntimeDivergenceDiagnostics
ScriptHookSdkFacade::SampleRuntimeDivergenceDiagnostics() noexcept {
    RuntimeDivergenceDiagnostics diagnostics;
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        diagnostics.entities.desiredCount =
            worldProxyEntries_.size();
        const auto now = TickMilliseconds();
        std::vector<float> entityPositionErrors;
        entityPositionErrors.reserve(worldProxyEntries_.size());
        constexpr auto kScriptOwned =
            static_cast<std::uint8_t>(
                WorldEntityStateFlag::ScriptOwned);
        for (const auto& [entityId, entry] : worldProxyEntries_) {
            const bool scriptOwned =
                (entry.state.flags & kScriptOwned) != 0U;
            diagnostics.entities.desiredScriptOwnedCount +=
                scriptOwned ? 1U : 0U;
            const auto local = worldEntityReplicas_.FindLocal(entityId);
            if (!local.has_value() ||
                ENTITY::DOES_ENTITY_EXIST(*local) == FALSE) {
                const auto missingAge =
                    entry.requestedAtMs == 0U ||
                            now < entry.requestedAtMs
                        ? 0U
                        : now - entry.requestedAtMs;
                const auto grace = scriptOwned ? 2'000U : 5'000U;
                if (missingAge <= grace) {
                    ++diagnostics.entities.pendingCount;
                    diagnostics.entities.pendingScriptOwnedCount +=
                        scriptOwned ? 1U : 0U;
                } else {
                    ++diagnostics.entities.missingCount;
                    diagnostics.entities.missingScriptOwnedCount +=
                        scriptOwned ? 1U : 0U;
                    if (missingAge >=
                        diagnostics.entities
                            .oldestMissingAgeMilliseconds) {
                        diagnostics.entities
                            .oldestMissingAgeMilliseconds = missingAge;
                        diagnostics.entities
                            .oldestMissingEntityId = entityId;
                    }
                }
                continue;
            }
            ++diagnostics.entities.liveCount;
            diagnostics.entities.liveScriptOwnedCount +=
                scriptOwned ? 1U : 0U;
            const auto position = ToBridgeVector(
                ENTITY::GET_ENTITY_COORDS(*local, TRUE, FALSE));
            const auto error = Distance(position, entry.state.position);
            if (!std::isfinite(error)) {
                continue;
            }
            entityPositionErrors.push_back(error);
            if (error >= kEntityDivergenceThresholdMeters) {
                ++diagnostics.entities.divergentCount;
                diagnostics.entities.divergentScriptOwnedCount +=
                    scriptOwned ? 1U : 0U;
            }
            if (error >
                diagnostics.entities.worstPositionErrorMeters) {
                diagnostics.entities.worstPositionErrorMeters = error;
                diagnostics.entities.worstEntityId = entityId;
            }
        }
        if (!entityPositionErrors.empty()) {
            std::ranges::sort(entityPositionErrors);
            const auto p95Index = std::min(
                entityPositionErrors.size() - 1U,
                static_cast<std::size_t>(std::ceil(
                    static_cast<double>(entityPositionErrors.size()) *
                    0.95)) -
                    1U);
            diagnostics.entities.positionErrorP95Meters =
                entityPositionErrors[p95Index];
        }

        const auto remote = replicas_.FindLocal(remotePlayerId_);
        if (remote.has_value() &&
            ENTITY::DOES_ENTITY_EXIST(*remote) != FALSE &&
            hasPreviousRemoteTarget_) {
            diagnostics.player.available = true;
            const auto position = ToBridgeVector(
                ENTITY::GET_ENTITY_COORDS(*remote, TRUE, FALSE));
            diagnostics.player.positionErrorMeters =
                Distance(position, previousRemoteTargetPosition_);
            diagnostics.player.rotationErrorDegrees =
                AbsoluteHeadingDifference(
                    ENTITY::GET_ENTITY_HEADING(*remote),
                    remoteDiagnosticsTargetHeading_);
            diagnostics.player.expectedGait =
                static_cast<std::uint8_t>(previousRemoteLocomotion_);
            const auto velocity = ToBridgeVector(
                ENTITY::GET_ENTITY_VELOCITY(*remote, FALSE));
            const auto horizontalSpeed = std::hypot(
                velocity.x,
                velocity.y);
            const auto observedGait =
                horizontalSpeed < 0.20F
                    ? RemoteLocomotion::Idle
                    : horizontalSpeed < 2.25F
                          ? RemoteLocomotion::Walk
                          : horizontalSpeed < 5.75F
                                ? RemoteLocomotion::Run
                                : RemoteLocomotion::Sprint;
            diagnostics.player.observedGait =
                static_cast<std::uint8_t>(observedGait);
        }

        for (const auto& channel : remotePlayerActionChannels_) {
            if (!channel.active || channel.actionId == 0U ||
                channel.receivedAtMs == 0U) {
                continue;
            }
            const auto freshness =
                now >= channel.receivedAtMs
                    ? now - channel.receivedAtMs
                    : 0U;
            if (!diagnostics.player.actionActive ||
                freshness >=
                    diagnostics.player.actionFreshnessMilliseconds) {
                diagnostics.player.actionActive = true;
                diagnostics.player.activeActionId = channel.actionId;
                diagnostics.player.actionFreshnessMilliseconds =
                    freshness;
            }
        }
#endif
    } catch (...) {
        return {};
    }
    return diagnostics;
}

void ScriptHookSdkFacade::MaintainRemoteMissionParticipant(
    const bool hidden) noexcept {
    try {
#if COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS
        const auto restoreEntity = [](
                                       const LocalEntityHandle handle,
                                       const bool wasVisible) {
            if (handle == 0 ||
                ENTITY::DOES_ENTITY_EXIST(handle) == FALSE) {
                return;
            }
            ENTITY::FREEZE_ENTITY_POSITION(handle, FALSE);
            ENTITY::SET_ENTITY_COLLISION(handle, TRUE, TRUE);
            ENTITY::SET_ENTITY_VISIBLE(
                handle,
                wasVisible ? TRUE : FALSE);
            if (wasVisible) {
                ENTITY::RESET_ENTITY_ALPHA(handle);
            }
        };
        const auto hideEntity = [](const LocalEntityHandle handle) {
            if (handle == 0 ||
                ENTITY::DOES_ENTITY_EXIST(handle) == FALSE) {
                return;
            }
            ENTITY::FREEZE_ENTITY_POSITION(handle, TRUE);
            ENTITY::SET_ENTITY_COLLISION(handle, FALSE, TRUE);
            ENTITY::SET_ENTITY_VISIBLE(handle, FALSE);
            ENTITY::SET_ENTITY_ALPHA(handle, 0, FALSE);
        };

        if (!hidden) {
            if (!remoteMissionParticipantHidden_ &&
                remoteMissionParticipantPed_ == 0 &&
                remoteMissionParticipantMount_ == 0) {
                return;
            }
            restoreEntity(
                remoteMissionParticipantPed_,
                remoteMissionParticipantWasVisible_);
            restoreEntity(
                remoteMissionParticipantMount_,
                remoteMissionParticipantMountWasVisible_);
            remoteMissionParticipantHidden_ = false;
            remoteMissionParticipantPed_ = 0;
            remoteMissionParticipantMount_ = 0;
            remoteMissionParticipantWasVisible_ = true;
            remoteMissionParticipantMountWasVisible_ = true;
            Log(
                "[MISSION_SPECTATOR] host restored remote guest proxy after vanilla scene");
            return;
        }

        const auto remotePlayer =
            replicas_.FindLocal(remotePlayerId_).value_or(0);
        if (remoteMissionParticipantPed_ != 0 &&
            remoteMissionParticipantPed_ != remotePlayer) {
            restoreEntity(
                remoteMissionParticipantPed_,
                remoteMissionParticipantWasVisible_);
            remoteMissionParticipantPed_ = 0;
        }
        if (remotePlayer != 0 &&
            ENTITY::DOES_ENTITY_EXIST(remotePlayer) != FALSE &&
            remoteMissionParticipantPed_ == 0) {
            remoteMissionParticipantPed_ = remotePlayer;
            remoteMissionParticipantWasVisible_ =
                ENTITY::IS_ENTITY_VISIBLE(remotePlayer) != FALSE ||
                ENTITY::GET_ENTITY_ALPHA(remotePlayer) > 0;
        }

        const auto remoteMount =
            remoteMountId_.IsValid()
                ? remoteMountReplicas_.FindLocal(remoteMountId_).value_or(0)
                : 0;
        if (remoteMissionParticipantMount_ != 0 &&
            remoteMissionParticipantMount_ != remoteMount) {
            restoreEntity(
                remoteMissionParticipantMount_,
                remoteMissionParticipantMountWasVisible_);
            remoteMissionParticipantMount_ = 0;
        }
        if (remoteMount != 0 &&
            ENTITY::DOES_ENTITY_EXIST(remoteMount) != FALSE &&
            remoteMissionParticipantMount_ == 0) {
            remoteMissionParticipantMount_ = remoteMount;
            remoteMissionParticipantMountWasVisible_ =
                ENTITY::IS_ENTITY_VISIBLE(remoteMount) != FALSE ||
                ENTITY::GET_ENTITY_ALPHA(remoteMount) > 0;
        }

        if (!remoteMissionParticipantHidden_) {
            remoteMissionParticipantHidden_ = true;
            ClearRemotePlayerActions();
            if (remoteMissionParticipantPed_ != 0 &&
                ENTITY::DOES_ENTITY_EXIST(
                    remoteMissionParticipantPed_) != FALSE) {
                AI::CLEAR_PED_TASKS(
                    static_cast<Ped>(remoteMissionParticipantPed_),
                    TRUE,
                    TRUE);
            }
            Log(
                "[MISSION_SPECTATOR] host froze and hid remote guest proxy for vanilla scene ownership");
        }
        hideEntity(remoteMissionParticipantPed_);
        hideEntity(remoteMissionParticipantMount_);
#else
        (void)hidden;
#endif
    } catch (...) {
        Log(
            "[ERROR][MISSION_SPECTATOR] failed to maintain remote participant scene isolation");
    }
}

void ScriptHookSdkFacade::RequestCheckpointRetry() noexcept {
    Log("checkpoint retry requested; native binding is not yet validated");
}

void ScriptHookSdkFacade::Log(const std::string_view text) noexcept {
    try {
        std::string sanitized{text.substr(0U, 2'048U)};
        std::replace(
            sanitized.begin(),
            sanitized.end(),
            '\r',
            ' ');
        std::replace(
            sanitized.begin(),
            sanitized.end(),
            '\n',
            ' ');
        std::string line{"[CoopStoryBridge] "};
        line.append(sanitized);
        line.push_back('\n');
        ::OutputDebugStringA(line.c_str());
        AppendPersistentBridgeLog(sanitized);
    } catch (...) {
        // Logging must be safe during ASI unload.
    }
}

void ScriptHookSdkFacade::WaitForNextTick() noexcept {
    scriptWait(0U);
}

}  // namespace coopstory::bridge::sdk
