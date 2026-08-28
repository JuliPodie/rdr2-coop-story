#include "coopstory/bridge/BridgeRuntime.hpp"
#include "coopstory/bridge/AnimationReplicationCodec.hpp"
#include "coopstory/bridge/CampaignMissionCatalog.hpp"
#include "coopstory/bridge/EntityRegistry.hpp"
#include "coopstory/bridge/FrameCodec.hpp"
#include "coopstory/bridge/MenuController.hpp"
#include "coopstory/bridge/MissionBubble.hpp"
#include "coopstory/bridge/PlayerRuntime.hpp"
#include "coopstory/bridge/PlayerActionPolicy.hpp"
#include "coopstory/bridge/RemoteMotion.hpp"
#include "coopstory/bridge/SessionMenuController.hpp"
#include "coopstory/bridge/Telemetry.hpp"
#include "coopstory/bridge/VersionGate.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using namespace coopstory::bridge;

void Check(
    const bool condition,
    const std::string_view expression,
    const std::string_view file,
    const int line) {
    if (!condition) {
        throw std::runtime_error(
            std::string(file) + ":" + std::to_string(line) +
            " check failed: " + std::string(expression));
    }
}

#define CHECK(expression) \
    Check(static_cast<bool>(expression), #expression, __FILE__, __LINE__)

void FrameCodecRoundTrip() {
    Frame source;
    source.header.type = MessageType::PlayerState;
    source.header.sequence = 0x10203040U;
    source.header.tick = 0x0102030405060708ULL;
    source.payload = {0xAAU, 0xBBU, 0xCCU};

    const auto bytes = FrameCodec::Encode(source);
    CHECK(bytes.size() == kFrameHeaderSize + 3U);
    CHECK(bytes[0] == static_cast<std::uint8_t>('R'));
    CHECK(bytes[1] == static_cast<std::uint8_t>('2'));
    CHECK(bytes[2] == static_cast<std::uint8_t>('C'));
    CHECK(bytes[3] == static_cast<std::uint8_t>('P'));
    CHECK(bytes[4] == 27U);
    CHECK(bytes[5] == 0U);
    CHECK(bytes[6] == 4U);
    CHECK(bytes[7] == 0U);
    CHECK(bytes[20] == 3U);

    const auto decoded = FrameCodec::DecodeOne(bytes);
    CHECK(decoded.status == DecodeStatus::Complete);
    CHECK(decoded.consumed == bytes.size());
    CHECK(decoded.frame.has_value());
    CHECK(decoded.frame->header.type == MessageType::PlayerState);
    CHECK(decoded.frame->header.sequence == source.header.sequence);
    CHECK(decoded.frame->header.tick == source.header.tick);
    CHECK(decoded.frame->payload == source.payload);

    const CampaignCapabilityPayload capability{
        CampaignCapabilityKind::Recipe, 0x366089E7U, 91U, 1'700'000'000'000LL};
    const auto capabilityBytes = EncodeCampaignCapability(capability);
    CHECK(capabilityBytes.size() == kCampaignCapabilityPayloadSize);
    const auto decodedCapability = DecodeCampaignCapability(capabilityBytes);
    CHECK(decodedCapability.has_value());
    CHECK(decodedCapability->kind == capability.kind);
    CHECK(decodedCapability->recordHash == capability.recordHash);
    CHECK(decodedCapability->hostEventId == capability.hostEventId);
    CHECK(decodedCapability->grantedAtUnixMilliseconds == capability.grantedAtUnixMilliseconds);

    FrameStreamDecoder stream;
    stream.Append(
        std::span<const std::uint8_t>{bytes.data(), 7U});
    CHECK(!stream.Pop().has_value());
    stream.Append(
        std::span<const std::uint8_t>{
            bytes.data() + 7U,
            bytes.size() - 7U});
    CHECK(stream.Pop().has_value());

    auto malformed = bytes;
    malformed[0] = 0U;
    CHECK(
        FrameCodec::DecodeOne(malformed).status ==
        DecodeStatus::Invalid);

    Frame oversized;
    oversized.payload.resize(
        static_cast<std::size_t>(kMaximumFramePayload) + 1U);
    bool rejected{};
    try {
        (void)FrameCodec::Encode(oversized);
    } catch (const std::length_error&) {
        rejected = true;
    }
    CHECK(rejected);
}

void CampaignMissionCatalogIsExplicitAndBound() {
    const auto fud1 = FindCampaignMission(kFud1MissionId);
    CHECK(fud1.has_value());
    CHECK(kFud1MissionId == 0x979E7766U);
    CHECK(fud1->scriptName == "FUD1");
    CHECK(fud1->displayName == "The New South");
    const auto hunt1 = FindCampaignMission(kHunt1MissionId);
    CHECK(hunt1.has_value());
    CHECK(kHunt1MissionId == CampaignMissionId("HNT1"));
    CHECK(hunt1->scriptName == "HNT1");
    CHECK(hunt1->runtimeScriptName == "hunting1");
    CHECK(hunt1->displayName == "Exit Pursued by a Bruised Ego");
    CHECK(HasVerifiedCampaignCompletionMapping(kFud1MissionId));
    CHECK(HasVerifiedCampaignCompletionMapping(kHunt1MissionId));
    const auto fud1Rewards = CampaignMissionRewards(kFud1MissionId);
    CHECK(fud1Rewards.size() == 1U);
    CHECK(fud1Rewards.front().binding ==
        CampaignMissionRewardBinding::WeaponOwnership);
    CHECK(fud1Rewards.front().recordHash ==
        CampaignMissionId("WEAPON_FISHINGROD"));
    CHECK(fud1Rewards.front().amount == 1U);
    const auto sad3Rewards = CampaignMissionRewards(CampaignMissionId("SAD3"));
    CHECK(sad3Rewards.size() == 1U);
    CHECK(sad3Rewards.front().binding ==
        CampaignMissionRewardBinding::WeaponOwnership);
    CHECK(sad3Rewards.front().recordHash ==
        CampaignMissionId("WEAPON_SNIPERRIFLE_CARCANO"));
    CHECK(sad3Rewards.front().amount == 50U);
    const auto mar8Rewards = CampaignMissionRewards(CampaignMissionId("MAR8"));
    CHECK(mar8Rewards.size() == 1U);
    CHECK(mar8Rewards.front().binding ==
        CampaignMissionRewardBinding::WeaponOwnership);
    CHECK(mar8Rewards.front().recordHash ==
        CampaignMissionId("WEAPON_KIT_BINOCULARS"));
    CHECK(mar8Rewards.front().amount == 0U);
    const auto ab21Rewards = CampaignMissionRewards(CampaignMissionId("AB21"));
    CHECK(ab21Rewards.size() == 1U);
    CHECK(ab21Rewards.front().binding ==
        CampaignMissionRewardBinding::InventoryItem);
    CHECK(ab21Rewards.front().recordHash ==
        CampaignMissionId("DOCUMENT_LETTER_SADIE_TELEGRAM"));
    CHECK(ab21Rewards.front().amount == 1U);
    const auto rabi1Rewards = CampaignMissionRewards(CampaignMissionId("RABI1"));
    // The fishing rod is temporarily lent by RABI1 and removed by the same
    // script, so it is intentionally not a permanent completion reward.
    CHECK(rabi1Rewards.empty());
    const auto wnt4Rewards = CampaignMissionRewards(CampaignMissionId("WNT4"));
    CHECK(wnt4Rewards.size() == 2U);
    CHECK(wnt4Rewards.front().binding ==
        CampaignMissionRewardBinding::WeaponOwnership);
    CHECK(wnt4Rewards.front().recordHash ==
        CampaignMissionId("WEAPON_REPEATER_CARBINE"));
    CHECK(wnt4Rewards.front().amount == 99U);
    CHECK(wnt4Rewards[1].binding ==
        CampaignMissionRewardBinding::WeaponOwnership);
    CHECK(wnt4Rewards[1].recordHash == CampaignMissionId("WEAPON_LASSO"));
    CHECK(wnt4Rewards[1].amount == 1U);
    const auto sen1Rewards = CampaignMissionRewards(CampaignMissionId("SEN1"));
    CHECK(sen1Rewards.size() == 1U);
    CHECK(sen1Rewards[0].binding ==
        CampaignMissionRewardBinding::WeaponOwnership);
    CHECK(sen1Rewards[0].recordHash ==
        CampaignMissionId("WEAPON_THROWN_TOMAHAWK"));
    const auto dst1Rewards = CampaignMissionRewards(CampaignMissionId("DST1"));
    CHECK(dst1Rewards.size() == 2U);
    CHECK(dst1Rewards[0].recordHash ==
        CampaignMissionId("WEAPON_SHOTGUN_DOUBLEBARREL"));
    CHECK(dst1Rewards[1].recordHash ==
        CampaignMissionId("WEAPON_THROWN_THROWING_KNIVES"));
    const auto ind3Rewards = CampaignMissionRewards(CampaignMissionId("IND3"));
    CHECK(ind3Rewards.size() == 1U);
    CHECK(ind3Rewards[0].recordHash ==
        CampaignMissionId("WEAPON_SHOTGUN_SEMIAUTO"));
    const auto mud4Rewards = CampaignMissionRewards(CampaignMissionId("MUD4"));
    CHECK(mud4Rewards.size() == 1U);
    CHECK(mud4Rewards[0].recordHash ==
        CampaignMissionId("WEAPON_SNIPERRIFLE_ROLLINGBLOCK"));
    const auto tre1Rewards = CampaignMissionRewards(CampaignMissionId("TRE1"));
    CHECK(tre1Rewards.size() == 1U);
    CHECK(tre1Rewards[0].recordHash ==
        CampaignMissionId("WEAPON_SNIPERRIFLE_ROLLINGBLOCK_EXOTIC"));
    const auto mud1Rewards = CampaignMissionRewards(CampaignMissionId("MUD1"));
    CHECK(mud1Rewards.size() == 1U);
    CHECK(mud1Rewards[0].binding ==
        CampaignMissionRewardBinding::WeaponShopEligibility);
    CHECK(mud1Rewards[0].recordHash ==
        CampaignMissionId("WEAPON_THROWN_TOMAHAWK"));
    const auto gry1Rewards = CampaignMissionRewards(CampaignMissionId("GRY1"));
    CHECK(gry1Rewards.size() == 1U);
    CHECK(gry1Rewards[0].binding ==
        CampaignMissionRewardBinding::WeaponShopEligibility);
    CHECK(gry1Rewards[0].recordHash ==
        CampaignMissionId("WEAPON_REPEATER_EVANS"));
    const auto hunt1Rewards = CampaignMissionRewards(kHunt1MissionId);
    CHECK(hunt1Rewards.size() == 2U);
    CHECK(hunt1Rewards[0].binding ==
        CampaignMissionRewardBinding::InventoryItem);
    CHECK(hunt1Rewards[0].recordHash ==
        CampaignMissionId("DOCUMENT_MAP_LEGENDARY_ANIMALS"));
    CHECK(hunt1Rewards[0].amount == 1U);
    CHECK(hunt1Rewards[1].binding ==
        CampaignMissionRewardBinding::UnlockVisible);
    CHECK(hunt1Rewards[1].recordHash ==
        CampaignMissionId("SP_CHAL_HUNT_ROOT"));
    CHECK(static_cast<std::uint8_t>(
              CampaignMissionRewardBinding::RecipeUnlock) == 4U);
    CHECK(static_cast<std::uint8_t>(
              CampaignMissionRewardBinding::UnlockEntitlement) == 5U);
    CHECK(static_cast<std::uint8_t>(
              CampaignMissionRewardBinding::WeaponShopEligibility) == 6U);
    CHECK(std::size(kCampaignMissionCatalog) > 2U);
    for (std::size_t index{}; index < std::size(kCampaignMissionCatalog); ++index) {
        const auto& definition = kCampaignMissionCatalog[index];
        CHECK(definition.missionId == CampaignMissionId(definition.scriptName));
        CHECK(!definition.runtimeScriptName.empty());
        CHECK(HasVerifiedCampaignCompletionMapping(definition.missionId));
        for (std::size_t other = index + 1U;
             other < std::size(kCampaignMissionCatalog);
             ++other) {
            CHECK(definition.missionId != kCampaignMissionCatalog[other].missionId);
        }
    }
    CHECK(!FindCampaignMission(0xFFFFFFFFU).has_value());
}

void PlayerActionEpochPolicy() {
    using Decision = RemotePlayerActionEpochDecision;

    CHECK(
        EvaluateRemotePlayerActionEpoch(
            0U,
            0U,
            10U,
            1U,
            PlayerActionPhase::Begin) ==
        Decision::AcceptInitial);
    CHECK(
        EvaluateRemotePlayerActionEpoch(
            10U,
            1U,
            10U,
            2U,
            PlayerActionPhase::End) ==
        Decision::AcceptRevision);
    CHECK(
        EvaluateRemotePlayerActionEpoch(
            10U,
            2U,
            10U,
            2U,
            PlayerActionPhase::End) ==
        Decision::IgnoreStaleRevision);

    // A delayed terminal from an older epoch must not kill action 12.
    CHECK(
        EvaluateRemotePlayerActionEpoch(
            12U,
            1U,
            10U,
            2U,
            PlayerActionPhase::End) ==
        Decision::IgnoreForeignTerminal);
    CHECK(
        EvaluateRemotePlayerActionEpoch(
            12U,
            1U,
            13U,
            1U,
            PlayerActionPhase::Sustain) ==
        Decision::IgnoreForeignContinuation);
    CHECK(
        EvaluateRemotePlayerActionEpoch(
            12U,
            1U,
            13U,
            1U,
            PlayerActionPhase::Begin) ==
        Decision::AcceptNewBegin);
    CHECK(
        EvaluateRemotePlayerActionEpoch(
            12U,
            1U,
            11U,
            1U,
            PlayerActionPhase::Begin) ==
        Decision::IgnoreOlderBegin);

    CHECK(IsNewerPlayerActionId(1U, 0xFFFFFFFFU));
    CHECK(!IsNewerPlayerActionId(0xFFFFFFFFU, 1U));

    CHECK(!ShouldStartNativeLassoTask(
        PlayerActionKind::Lasso,
        PlayerActionPhase::Begin,
        true,
        true,
        false));
    CHECK(ShouldStartNativeLassoTask(
        PlayerActionKind::Lasso,
        PlayerActionPhase::Sustain,
        true,
        false,
        true));
    CHECK(!ShouldStartNativeLassoTask(
        PlayerActionKind::Lasso,
        PlayerActionPhase::Sustain,
        true,
        true,
        false));
    CHECK(!ShouldStartNativeLassoTask(
        PlayerActionKind::Lasso,
        PlayerActionPhase::Sustain,
        true,
        false,
        false));
    CHECK(!ShouldStartNativeLassoTask(
        PlayerActionKind::Lasso,
        PlayerActionPhase::End,
        true,
        false,
        true));
    CHECK(!ShouldStartNativeLassoTask(
        PlayerActionKind::Lasso,
        PlayerActionPhase::Begin,
        false,
        false,
        false));
    CHECK(!ShouldStartNativeLassoTask(
        PlayerActionKind::Knockdown,
        PlayerActionPhase::Begin,
        true,
        true,
        true));
}

void GuestMissionGatePolicy() {
    // A connected lease alone must not occupy RDR2's process-global mission
    // flag. Once the host publishes an authoritative mission, the guest gate
    // stays occupied so a delayed private Story VM cannot start the same
    // mission and later report Gang Abandoned.
    CHECK(!ShouldAssertGuestMissionGate(
        true, false, false, false, false));
    CHECK(ShouldAssertGuestMissionGate(
        true, true, false, false, false));
    CHECK(ShouldAssertGuestMissionGate(
        true, false, true, false, false));
    CHECK(ShouldAssertGuestMissionGate(
        true, false, false, true, false));
    CHECK(ShouldAssertGuestMissionGate(
        true, false, false, false, true));
    CHECK(!ShouldAssertGuestMissionGate(
        false, true, true, true, true));
}

void PeerMountPullInputPolicy() {
    CHECK(ShouldPublishPeerMountPull(
        true, true, true, true, false, false));
    CHECK(!ShouldPublishPeerMountPull(
        true, true, true, true, true, false));
    CHECK(!ShouldPublishPeerMountPull(
        true, true, true, true, false, true));
    CHECK(!ShouldPublishPeerMountPull(
        false, true, true, true, false, false));
    CHECK(!ShouldPublishPeerMountPull(
        true, false, true, true, false, false));
    CHECK(!ShouldPublishPeerMountPull(
        true, true, false, true, false, false));
    CHECK(!ShouldPublishPeerMountPull(
        true, true, true, false, false, false));
}

void PayloadContracts() {
    const PlayerStatePayload source{
        NetEntityId::Compose(0x11223344U, 0x55667788U),
        PlayerSlot::Guest,
        PlayerLifecycle::Downed,
        {1.0F, 2.0F, 3.0F},
        {4.0F, 5.0F, 6.0F},
        270.0F,
        0.25F,
        7U |
            static_cast<std::uint32_t>(
                PlayerStateFlag::MeleeCombat) |
            static_cast<std::uint32_t>(
                PlayerStateFlag::SyntheticTest) |
            static_cast<std::uint32_t>(
                PlayerStateFlag::InWater) |
            static_cast<std::uint32_t>(
                PlayerStateFlag::Swimming),
        {},
        0U,
        15.0F,
        2.5F,
        -0.5F,
        2.0F,
        7U,
        9U,
        PlayerTraversalKind::Jump,
        PlayerLocomotionMode::Traversal,
        {1.0F, 2.0F, 3.0F},
        20.0F};
    const auto bytes = EncodePlayerState(source);
    CHECK(bytes.size() == kPlayerStatePayloadSize);
    CHECK(bytes[0] == 0x88U);
    CHECK(bytes[4] == 0x44U);
    const auto decoded = DecodePlayerState(bytes);
    CHECK(decoded.has_value());
    CHECK(decoded->entityId == source.entityId);
    CHECK(decoded->slot == PlayerSlot::Guest);
    CHECK(decoded->lifecycle == PlayerLifecycle::Downed);
    CHECK(decoded->position.z == 3.0F);
    CHECK(
        (decoded->flags &
         static_cast<std::uint32_t>(
             PlayerStateFlag::MeleeCombat)) != 0U);
    CHECK(
        (decoded->flags &
             static_cast<std::uint32_t>(
                 PlayerStateFlag::SyntheticTest)) != 0U);
    CHECK(
        static_cast<std::uint32_t>(PlayerStateFlag::Jumping) ==
        (1U << 8U));
    CHECK(
        static_cast<std::uint32_t>(PlayerStateFlag::Climbing) ==
        (1U << 9U));
    CHECK(
        static_cast<std::uint32_t>(PlayerStateFlag::StealthMovement) ==
        (1U << 10U));
    CHECK(
        static_cast<std::uint32_t>(PlayerStateFlag::PeerCombatTarget) ==
        (1U << 11U));
    CHECK(
        static_cast<std::uint32_t>(PlayerStateFlag::PeerLassoActive) ==
        (1U << 12U));
    CHECK(
        static_cast<std::uint32_t>(PlayerStateFlag::MeleeBlocking) ==
        (1U << 13U));
    CHECK(
        static_cast<std::uint32_t>(PlayerStateFlag::MeleeGrappling) ==
        (1U << 14U));
    CHECK(
        static_cast<std::uint32_t>(PlayerStateFlag::PeerKnockdown) ==
        (1U << 15U));
    CHECK(
        static_cast<std::uint32_t>(PlayerStateFlag::InCover) ==
        (1U << 16U));
    CHECK(
        static_cast<std::uint32_t>(PlayerStateFlag::GoingIntoCover) ==
        (1U << 17U));
    CHECK(
        static_cast<std::uint32_t>(PlayerStateFlag::CoverFacingLeft) ==
        (1U << 18U));
    CHECK(
        static_cast<std::uint32_t>(PlayerStateFlag::AimingFromCover) ==
        (1U << 19U));
    CHECK(
        static_cast<std::uint32_t>(PlayerStateFlag::InWater) ==
        (1U << 20U));
    CHECK(
        static_cast<std::uint32_t>(PlayerStateFlag::Swimming) ==
        (1U << 21U));
    CHECK(
        static_cast<std::uint32_t>(
            PlayerStateFlag::SwimmingUnderwater) ==
        (1U << 22U));
    CHECK(
        (decoded->flags & static_cast<std::uint32_t>(
             PlayerStateFlag::InWater)) != 0U);
    CHECK(
        (decoded->flags & static_cast<std::uint32_t>(
             PlayerStateFlag::Swimming)) != 0U);
    CHECK(decoded->locomotionEpoch == 7U);
    CHECK(decoded->traversalActionId == 9U);
    CHECK(decoded->traversalKind == PlayerTraversalKind::Jump);
    CHECK(decoded->locomotionMode == PlayerLocomotionMode::Traversal);
    CHECK(decoded->traversalAnchor.z == 3.0F);

    const PlayerTraversalPayload traversal{
        source.entityId,
        PlayerSlot::Guest,
        PlayerTraversalKind::Climb,
        9U,
        2U,
        7U,
        static_cast<std::uint32_t>(
            PlayerTraversalFlag::InputEdgeDetected) |
            static_cast<std::uint32_t>(
                PlayerTraversalFlag::ObstacleValid) |
            static_cast<std::uint32_t>(
                PlayerTraversalFlag::ExpectedLandingValid),
        20.0F,
        {1.0F, 2.0F, 3.0F},
        {4.0F, 5.0F, 0.0F},
        {1.5F, 2.5F, 3.5F},
        {0.0F, -1.0F, 0.0F},
        3.5F,
        {2.0F, 3.0F, 4.0F}};
    const auto traversalBytes = EncodePlayerTraversal(traversal);
    CHECK(traversalBytes.size() == kPlayerTraversalPayloadSize);
    const auto decodedTraversal =
        DecodePlayerTraversal(traversalBytes);
    CHECK(decodedTraversal.has_value());
    CHECK(decodedTraversal->actionId == traversal.actionId);
    CHECK(decodedTraversal->revision == traversal.revision);
    CHECK(decodedTraversal->kind == PlayerTraversalKind::Climb);
    CHECK(decodedTraversal->expectedLanding.z == 4.0F);

    const auto actionTargetId =
        NetEntityId::Compose(0x11223344U, 1U);
    constexpr auto kActionFlags =
        static_cast<std::uint32_t>(PlayerActionFlag::Intent) |
        static_cast<std::uint32_t>(
            PlayerActionFlag::TargetEntityValid) |
        static_cast<std::uint32_t>(
            PlayerActionFlag::TargetPointValid) |
        static_cast<std::uint32_t>(
            PlayerActionFlag::ActorAnchorValid) |
        static_cast<std::uint32_t>(PlayerActionFlag::Persistent) |
        static_cast<std::uint32_t>(
            PlayerActionFlag::PhysicalTargetEffect) |
        static_cast<std::uint32_t>(PlayerActionFlag::VariantValid) |
        static_cast<std::uint32_t>(
            PlayerActionFlag::AnimationSampleValid) |
        static_cast<std::uint32_t>(
            PlayerActionFlag::NormalizedPhaseValid);
    const PlayerActionPayload action{
        source.entityId,
        actionTargetId,
        9U,
        11U,
        2U,
        PlayerSlot::Guest,
        PlayerSlot::Guest,
        PlayerActionKind::Grapple,
        PlayerActionPhase::Active,
        kActionFlags,
        1'000U,
        250U,
        0xAABBCCDDU,
        0x01020304U,
        77U,
        {1.0F, 2.0F, 3.0F},
        {4.0F, 5.0F, 6.0F},
        45.0F,
        0.25F};
    const auto actionBytes = EncodePlayerAction(action);
    CHECK(actionBytes.size() == kPlayerActionPayloadSize);
    CHECK(actionBytes[28] ==
          static_cast<std::uint8_t>(PlayerActionKind::Grapple));
    CHECK(actionBytes[29] ==
          static_cast<std::uint8_t>(PlayerActionPhase::Active));
    const auto decodedAction = DecodePlayerAction(actionBytes);
    CHECK(decodedAction.has_value());
    CHECK(decodedAction->actorEntityId == action.actorEntityId);
    CHECK(decodedAction->targetEntityId == action.targetEntityId);
    CHECK(decodedAction->sequence == 9U);
    CHECK(decodedAction->actionId == 11U);
    CHECK(decodedAction->revision == 2U);
    CHECK(decodedAction->kind == PlayerActionKind::Grapple);
    CHECK(decodedAction->phase == PlayerActionPhase::Active);
    CHECK(decodedAction->normalizedPhase == 0.25F);

    auto actionWithReserved = actionBytes;
    actionWithReserved[30] = 1U;
    CHECK(!DecodePlayerAction(actionWithReserved).has_value());
    auto actionWithoutTarget = actionBytes;
    std::fill_n(
        actionWithoutTarget.begin() + 8,
        8,
        std::uint8_t{0U});
    CHECK(!DecodePlayerAction(actionWithoutTarget).has_value());
    auto actionWithBothAuthorities = action;
    actionWithBothAuthorities.flags |=
        static_cast<std::uint32_t>(
            PlayerActionFlag::Authoritative);
    bool bothAuthoritiesRejected{};
    try {
        (void)EncodePlayerAction(actionWithBothAuthorities);
    } catch (const std::invalid_argument&) {
        bothAuthoritiesRejected = true;
    }
    CHECK(bothAuthoritiesRejected);
    auto actionWithInvalidPhase = action;
    actionWithInvalidPhase.normalizedPhase =
        std::numeric_limits<float>::quiet_NaN();
    bool invalidPhaseRejected{};
    try {
        (void)EncodePlayerAction(actionWithInvalidPhase);
    } catch (const std::invalid_argument&) {
        invalidPhaseRejected = true;
    }
    CHECK(invalidPhaseRejected);
    auto invalidSnapshot = action;
    invalidSnapshot.flags |=
        static_cast<std::uint32_t>(
            PlayerActionFlag::ResyncSnapshot);
    bool invalidSnapshotRejected{};
    try {
        (void)EncodePlayerAction(invalidSnapshot);
    } catch (const std::invalid_argument&) {
        invalidSnapshotRejected = true;
    }
    CHECK(invalidSnapshotRejected);

    const InteractionIntentPayload reviveIntent{
        source.entityId,
        actionTargetId,
        {},
        71U,
        1U,
        PlayerSlot::Guest,
        InteractionKind::Revive,
        InteractionIntentPhase::Begin,
        static_cast<std::uint8_t>(
            InteractionIntentFlag::TargetPlayer) |
            static_cast<std::uint8_t>(
                InteractionIntentFlag::HoldRequired),
        4'000U};
    const auto reviveIntentBytes =
        EncodeInteractionIntent(reviveIntent);
    CHECK(reviveIntentBytes.size() == kInteractionIntentPayloadSize);
    const auto decodedReviveIntent =
        DecodeInteractionIntent(reviveIntentBytes);
    CHECK(decodedReviveIntent.has_value());
    CHECK(decodedReviveIntent->interactionId == 71U);
    CHECK(decodedReviveIntent->kind == InteractionKind::Revive);
    auto invalidIntentReserved = reviveIntentBytes;
    invalidIntentReserved[34] = 1U;
    CHECK(!DecodeInteractionIntent(invalidIntentReserved).has_value());

    const InteractionResultPayload reviveResult{
        source.entityId,
        actionTargetId,
        {},
        71U,
        11U,
        InteractionKind::Revive,
        InteractionResultStatus::Completed,
        InteractionRejectReason::None,
        static_cast<std::uint16_t>(
            InteractionResultFlag::Authoritative) |
            static_cast<std::uint16_t>(
                InteractionResultFlag::StateChanged) |
            static_cast<std::uint16_t>(
                InteractionResultFlag::HoldRequired),
        4'000U,
        4'000U};
    const auto reviveResultBytes =
        EncodeInteractionResult(reviveResult);
    CHECK(reviveResultBytes.size() == kInteractionResultPayloadSize);
    const auto decodedReviveResult =
        DecodeInteractionResult(reviveResultBytes);
    CHECK(decodedReviveResult.has_value());
    CHECK(decodedReviveResult->status ==
          InteractionResultStatus::Completed);
    auto invalidResultReserved = reviveResultBytes;
    invalidResultReserved[33] = 1U;
    CHECK(!DecodeInteractionResult(invalidResultReserved).has_value());

    const RestraintStatePayload hogtiedState{
        actionTargetId,
        source.entityId,
        72U,
        3U,
        PlayerRestraintState::Hogtied,
        static_cast<std::uint8_t>(
            RestraintStateFlag::Authoritative) |
            static_cast<std::uint8_t>(
                RestraintStateFlag::EngineOwned)};
    const auto hogtiedBytes = EncodeRestraintState(hogtiedState);
    CHECK(hogtiedBytes.size() == kRestraintStatePayloadSize);
    const auto decodedHogtied = DecodeRestraintState(hogtiedBytes);
    CHECK(decodedHogtied.has_value());
    CHECK(decodedHogtied->state == PlayerRestraintState::Hogtied);
    auto invalidRestraintReserved = hogtiedBytes;
    invalidRestraintReserved[26] = 1U;
    CHECK(!DecodeRestraintState(invalidRestraintReserved).has_value());

    const InteractionIntentPayload emergencyIntent{
        source.entityId,
        source.entityId,
        {},
        73U,
        1U,
        PlayerSlot::Guest,
        InteractionKind::EmergencyRecover,
        InteractionIntentPhase::Begin,
        static_cast<std::uint8_t>(
            InteractionIntentFlag::TargetPlayer),
        0U};
    CHECK(DecodeInteractionIntent(
              EncodeInteractionIntent(emergencyIntent))
              .has_value());
    const InteractionResultPayload emergencyResult{
        source.entityId,
        source.entityId,
        {},
        73U,
        1U,
        InteractionKind::EmergencyRecover,
        InteractionResultStatus::Completed,
        InteractionRejectReason::None,
        static_cast<std::uint16_t>(
            InteractionResultFlag::Authoritative) |
            static_cast<std::uint16_t>(
                InteractionResultFlag::StateChanged),
        0U,
        0U};
    CHECK(DecodeInteractionResult(
              EncodeInteractionResult(emergencyResult))
              .has_value());

    const PlayerIdentityPayload identity{
        source.entityId,
        PlayerSlot::Guest,
        "Gracz_2"};
    const auto identityBytes =
        EncodePlayerIdentity(identity);
    CHECK(identityBytes.size() == 17U);
    CHECK(identityBytes[8] == 1U);
    CHECK(identityBytes[9] == 7U);
    const auto decodedIdentity =
        DecodePlayerIdentity(identityBytes);
    CHECK(decodedIdentity.has_value());
    CHECK(decodedIdentity->entityId == identity.entityId);
    CHECK(decodedIdentity->slot == PlayerSlot::Guest);
    CHECK(decodedIdentity->nickname == "Gracz_2");

    auto wrongIdentityLength = identityBytes;
    wrongIdentityLength[9] = 8U;
    CHECK(
        !DecodePlayerIdentity(
             wrongIdentityLength)
             .has_value());
    auto invalidIdentityUtf8 = identityBytes;
    invalidIdentityUtf8[10] = 0xFFU;
    CHECK(
        !DecodePlayerIdentity(
             invalidIdentityUtf8)
             .has_value());
    bool reservedNicknameRejected{};
    try {
        (void)EncodePlayerIdentity(
            PlayerIdentityPayload{
                source.entityId,
                PlayerSlot::Guest,
                "~HUD"});
    } catch (const std::invalid_argument&) {
        reservedNicknameRejected = true;
    }
    CHECK(reservedNicknameRejected);

    const PlayerMountStatePayload mount{
        source.entityId,
        NetEntityId::Compose(0x11223344U, 11U),
        PlayerSlot::Guest,
        static_cast<std::uint8_t>(
            PlayerMountStateFlag::Present) |
            static_cast<std::uint8_t>(
                PlayerMountStateFlag::Mounted) |
            static_cast<std::uint8_t>(
                PlayerMountStateFlag::BorrowedPeerMount),
        0xAABBCCDDU,
        {7.0F, 8.0F, 9.0F},
        {1.0F, 0.0F, 0.0F},
        180.0F,
        0.8F,
        3U};
    const auto mountBytes =
        EncodePlayerMountState(mount);
    CHECK(
        mountBytes.size() ==
        kPlayerMountStatePayloadSize);
    const auto decodedMount =
        DecodePlayerMountState(mountBytes);
    CHECK(decodedMount.has_value());
    CHECK(
        decodedMount->mountEntityId ==
        mount.mountEntityId);
    CHECK(decodedMount->generation == 3U);
    auto invalidMount = mountBytes;
    invalidMount[18] = 1U;
    CHECK(
        !DecodePlayerMountState(
             invalidMount)
             .has_value());
    auto borrowedWithoutRider = mountBytes;
    borrowedWithoutRider[17] =
        static_cast<std::uint8_t>(
            PlayerMountStateFlag::Present) |
        static_cast<std::uint8_t>(
            PlayerMountStateFlag::BorrowedPeerMount);
    CHECK(
        !DecodePlayerMountState(
             borrowedWithoutRider)
             .has_value());

    const WorldStatePayload world{
        21U,
        37U,
        12U,
        static_cast<std::uint8_t>(
            WorldStateFlag::WeatherValid),
        0x11223344U,
        0x55667788U,
        0.25F};
    const auto worldBytes = EncodeWorldState(world);
    CHECK(worldBytes.size() == kWorldStatePayloadSize);
    const auto decodedWorld = DecodeWorldState(worldBytes);
    CHECK(decodedWorld.has_value());
    CHECK(decodedWorld->hour == 21U);
    CHECK(decodedWorld->weatherTo == 0x55667788U);
    CHECK(decodedWorld->weatherBlend == 0.25F);
    auto invalidWorld = worldBytes;
    invalidWorld[0] = 24U;
    CHECK(!DecodeWorldState(invalidWorld).has_value());

    const MissionStatePayload mission{
        NetEntityId::Compose(0x01020304U, 0x05060708U),
        0x11223344U,
        0x55667788U,
        0xA1B2C3D4U,
        MissionPhase::Recovery,
        static_cast<std::uint8_t>(
            MissionStateFlag::MissionActive) |
            static_cast<std::uint8_t>(
                MissionStateFlag::AnchorValid) |
            static_cast<std::uint8_t>(
                MissionStateFlag::CheckpointRecovery),
        {1.0F, 2.0F, -4.0F},
        90.0F};
    const auto missionBytes = EncodeMissionState(mission);
    CHECK(missionBytes.size() == kMissionStatePayloadSize);
    CHECK(missionBytes[0] == 0x08U);
    CHECK(missionBytes[4] == 0x04U);
    CHECK(missionBytes[20] == 0x04U);
    CHECK(missionBytes[21] == 0x07U);
    CHECK(missionBytes[22] == 0U);
    CHECK(missionBytes[40] == 0U);
    const auto decodedMission =
        DecodeMissionState(missionBytes);
    CHECK(decodedMission.has_value());
    CHECK(*decodedMission == mission);
    CHECK(
        !DecodeMissionState(
             std::span<const std::uint8_t>{
                 missionBytes.data(),
                 missionBytes.size() - 1U})
             .has_value());

    const MissionProgressionPayload progression{
        0x2C3469EDU, 7U, 0x700000001ULL,
        MissionProgressionPhase::Eligibility,
        static_cast<std::uint8_t>(MissionProgressionFlag::GuestCanStart)};
    const auto progressionBytes = EncodeMissionProgression(progression);
    CHECK(progressionBytes.size() == kMissionProgressionPayloadSize);
    const auto decodedProgression = DecodeMissionProgression(progressionBytes);
    CHECK(decodedProgression.has_value());
    CHECK(*decodedProgression == progression);
    auto invalidProgression = progressionBytes;
    invalidProgression[17] = 0x80U;
    CHECK(!DecodeMissionProgression(invalidProgression).has_value());
    const MissionProgressionPayload completionProgression{
        0x2C3469EDU, 7U, 0x700000001ULL,
        MissionProgressionPhase::Completion,
        static_cast<std::uint8_t>(
            MissionProgressionFlag::VerifiedCompletionMapping),
        4U, 140};
    const auto completionProgressionBytes =
        EncodeMissionProgression(completionProgression);
    const auto decodedCompletionProgression =
        DecodeMissionProgression(completionProgressionBytes);
    CHECK(decodedCompletionProgression.has_value());
    CHECK(*decodedCompletionProgression == completionProgression);
    auto invalidCompletionProgression = completionProgression;
    invalidCompletionProgression.completionRating = 1U;
    bool invalidCompletionRejected{};
    try {
        (void)EncodeMissionProgression(invalidCompletionProgression);
    } catch (const std::invalid_argument&) {
        invalidCompletionRejected = true;
    }
    CHECK(invalidCompletionRejected);
    auto missionWithUnknownPhase = missionBytes;
    missionWithUnknownPhase[20] = 0xFFU;
    CHECK(
        !DecodeMissionState(
             missionWithUnknownPhase)
             .has_value());
    auto missionWithUnknownFlags = missionBytes;
    missionWithUnknownFlags[21] = 0x80U;
    CHECK(
        !DecodeMissionState(
             missionWithUnknownFlags)
             .has_value());
    auto missionWithReserved = missionBytes;
    missionWithReserved[22] = 1U;
    CHECK(
        !DecodeMissionState(
             missionWithReserved)
             .has_value());
    auto missionWithTrailingReserved = missionBytes;
    missionWithTrailingReserved[40] = 1U;
    CHECK(
        !DecodeMissionState(
             missionWithTrailingReserved)
             .has_value());
    auto missionWithoutEpoch = missionBytes;
    std::fill_n(
        missionWithoutEpoch.begin() + 8,
        4,
        std::uint8_t{0U});
    CHECK(
        !DecodeMissionState(
             missionWithoutEpoch)
             .has_value());
    auto missionWithoutRevision = missionBytes;
    std::fill_n(
        missionWithoutRevision.begin() + 12,
        4,
        std::uint8_t{0U});
    CHECK(
        !DecodeMissionState(
             missionWithoutRevision)
             .has_value());
    auto missionWithInvalidAnchor = missionBytes;
    missionWithInvalidAnchor[27] = 0x7FU;
    missionWithInvalidAnchor[26] = 0xC0U;
    CHECK(
        !DecodeMissionState(
             missionWithInvalidAnchor)
             .has_value());
    auto missionRecoveryFlagMissing = missionBytes;
    missionRecoveryFlagMissing[21] =
        static_cast<std::uint8_t>(
            MissionStateFlag::MissionActive) |
        static_cast<std::uint8_t>(
            MissionStateFlag::AnchorValid);
    CHECK(
        !DecodeMissionState(
             missionRecoveryFlagMissing)
             .has_value());
    bool invalidMissionRejected{};
    try {
        auto invalidMission = mission;
        invalidMission.flags &=
            ~static_cast<std::uint8_t>(
                MissionStateFlag::AnchorValid);
        (void)EncodeMissionState(invalidMission);
    } catch (const std::invalid_argument&) {
        invalidMissionRejected = true;
    }
    CHECK(invalidMissionRejected);

    const MissionCameraStatePayload missionCamera{
        mission.hostEntityId,
        mission.missionEpoch,
        7U,
        19U,
        static_cast<std::uint32_t>(
            MissionCameraStateFlag::Active) |
            static_cast<std::uint32_t>(
                MissionCameraStateFlag::SourceRenderingScriptCamera),
        {10.25F, -20.5F, 30.75F},
        {-12.0F, 0.5F, 181.0F},
        52.5F};
    const auto missionCameraBytes =
        EncodeMissionCameraState(missionCamera);
    CHECK(
        missionCameraBytes.size() ==
        kMissionCameraStatePayloadSize);
    const auto decodedMissionCamera =
        DecodeMissionCameraState(missionCameraBytes);
    CHECK(decodedMissionCamera.has_value());
    CHECK(*decodedMissionCamera == missionCamera);
    CHECK(
        !DecodeMissionCameraState(
             std::span<const std::uint8_t>{
                 missionCameraBytes.data(),
                 missionCameraBytes.size() - 1U})
             .has_value());
    auto invalidCameraFlags = missionCameraBytes;
    invalidCameraFlags[21] = 0x80U;
    CHECK(
        !DecodeMissionCameraState(
             invalidCameraFlags)
             .has_value());
    auto inactiveMissionCamera = missionCamera;
    inactiveMissionCamera.revision = 20U;
    inactiveMissionCamera.flags = 0U;
    inactiveMissionCamera.position = {};
    inactiveMissionCamera.rotation = {};
    inactiveMissionCamera.fieldOfView = 0.0F;
    CHECK(
        DecodeMissionCameraState(
            EncodeMissionCameraState(inactiveMissionCamera))
            .has_value());
    bool invalidMissionCameraRejected{};
    try {
        auto invalidMissionCamera = inactiveMissionCamera;
        invalidMissionCamera.position.x = 1.0F;
        (void)EncodeMissionCameraState(invalidMissionCamera);
    } catch (const std::invalid_argument&) {
        invalidMissionCameraRejected = true;
    }
    CHECK(invalidMissionCameraRejected);

    const MissionCinematicStatePayload cinematicState{
        mission.hostEntityId,
        mission.missionEpoch,
        7U,
        4U,
        mission.checkpointGeneration,
        MissionCinematicPhase::PrepareResume,
        static_cast<std::uint16_t>(
            MissionCinematicStateFlag::CameraExpected) |
            static_cast<std::uint16_t>(
                MissionCinematicStateFlag::AnchorValid),
        {20.0F, 30.0F, 40.0F},
        180.0F};
    const auto cinematicBytes =
        EncodeMissionCinematicState(cinematicState);
    CHECK(cinematicBytes.size() == kMissionCinematicStatePayloadSize);
    const auto decodedCinematic =
        DecodeMissionCinematicState(cinematicBytes);
    CHECK(decodedCinematic.has_value());
    CHECK(*decodedCinematic == cinematicState);
    auto invalidCinematicReserved = cinematicBytes;
    invalidCinematicReserved[44] = 1U;
    CHECK(!DecodeMissionCinematicState(invalidCinematicReserved).has_value());

    const MissionCinematicActionPayload cinematicAction{
        mission.hostEntityId,
        mission.missionEpoch,
        7U,
        9U,
        MissionCinematicActionKind::ResumeReady,
        static_cast<std::uint8_t>(PlayerSlot::Guest),
        static_cast<std::uint16_t>(
            MissionCinematicActionFlag::FallbackUsed)};
    const auto cinematicActionBytes =
        EncodeMissionCinematicAction(cinematicAction);
    CHECK(cinematicActionBytes.size() == kMissionCinematicActionPayloadSize);
    const auto decodedCinematicAction =
        DecodeMissionCinematicAction(cinematicActionBytes);
    CHECK(decodedCinematicAction.has_value());
    CHECK(*decodedCinematicAction == cinematicAction);
    auto invalidActionReserved = cinematicActionBytes;
    invalidActionReserved[31] = 1U;
    CHECK(!DecodeMissionCinematicAction(invalidActionReserved).has_value());

    const PlayerAppearanceStatePayload appearance{
        source.entityId,
        PlayerSlot::Guest,
        1U,
        static_cast<std::uint16_t>(
            PlayerAppearanceStateFlag::CompleteComponentSet) |
            static_cast<std::uint16_t>(
                PlayerAppearanceStateFlag::StoryMetaPed),
        4U,
        0xAABBCCDDU,
        0x0102030405060708ULL,
        {0x11111111U, 0x22222222U, 0x33333333U}};
    const auto appearanceBytes =
        EncodePlayerAppearanceState(appearance);
    CHECK(
        appearanceBytes.size() ==
        kPlayerAppearanceStateHeaderSize + 3U * sizeof(std::uint32_t));
    const auto decodedAppearance =
        DecodePlayerAppearanceState(appearanceBytes);
    CHECK(decodedAppearance.has_value());
    CHECK(decodedAppearance->entityId == appearance.entityId);
    CHECK(decodedAppearance->fingerprint == appearance.fingerprint);
    CHECK(decodedAppearance->componentHashes == appearance.componentHashes);
    auto appearanceReserved = appearanceBytes;
    appearanceReserved[22] = 1U;
    CHECK(!DecodePlayerAppearanceState(appearanceReserved).has_value());
    auto duplicateAppearance = appearance;
    duplicateAppearance.componentHashes =
        {0x11111111U, 0x11111111U};
    bool duplicateAppearanceRejected{};
    try {
        (void)EncodePlayerAppearanceState(duplicateAppearance);
    } catch (const std::invalid_argument&) {
        duplicateAppearanceRejected = true;
    }
    CHECK(duplicateAppearanceRejected);

    const AnimSceneReplicaStatePayload animScene{
        NetEntityId::Compose(0x11223344U, 1U),
        3U,
        2U,
        7U,
        15U,
        0x1234ABCDU,
        static_cast<std::uint32_t>(
            AnimSceneReplicaStateFlag::Active) |
            static_cast<std::uint32_t>(
                AnimSceneReplicaStateFlag::Running) |
            static_cast<std::uint32_t>(
                AnimSceneReplicaStateFlag::Loaded) |
            static_cast<std::uint32_t>(
                AnimSceneReplicaStateFlag::CameraActive) |
            static_cast<std::uint32_t>(
                AnimSceneReplicaStateFlag::OriginValid),
        0.42F,
        97.5F,
        1.0F,
        {10.0F, 20.0F, 30.0F},
        {0.0F, 0.0F, 90.0F},
        1U};
    const auto animSceneBytes = EncodeAnimSceneReplicaState(animScene);
    CHECK(animSceneBytes.size() == kAnimSceneReplicaStatePayloadSize);
    const auto decodedAnimScene =
        DecodeAnimSceneReplicaState(animSceneBytes);
    CHECK(decodedAnimScene.has_value());
    CHECK(*decodedAnimScene == animScene);
    auto animSceneReserved = animSceneBytes;
    animSceneReserved[70] = 1U;
    CHECK(!DecodeAnimSceneReplicaState(animSceneReserved).has_value());
    auto fallbackAnimScene = animScene;
    fallbackAnimScene.definitionRevision = 0U;
    CHECK(
        DecodeAnimSceneReplicaState(
            EncodeAnimSceneReplicaState(fallbackAnimScene)) ==
        fallbackAnimScene);
    auto invalidAnimScene = animScene;
    invalidAnimScene.phase = 1.5F;
    bool invalidAnimSceneRejected{};
    try {
        (void)EncodeAnimSceneReplicaState(invalidAnimScene);
    } catch (const std::invalid_argument&) {
        invalidAnimSceneRejected = true;
    }
    CHECK(invalidAnimSceneRejected);

    AnimSceneDefinitionPayload definition{
        animScene.hostEntityId,
        animScene.missionEpoch,
        animScene.cinematicGeneration,
        animScene.definitionRevision,
        animScene.dictionaryHash,
        0U,
        0U,
        animScene.durationSeconds,
        0x10203040U,
        0x03U,
        "script@story@intro",
        "pl_main",
        {
            {"Arthur",
             NetEntityId::Compose(0x11223344U, 2U),
             0xAABBCCDDU,
             AnimSceneRoleKind::Ped,
             static_cast<std::uint16_t>(
                 AnimSceneRoleFlag::Required) |
                 static_cast<std::uint16_t>(
                     AnimSceneRoleFlag::Player),
             0x01U},
            {"Dutch",
             NetEntityId::Compose(0x11223344U, 3U),
             0x10203040U,
             AnimSceneRoleKind::Horse,
             static_cast<std::uint16_t>(
                 AnimSceneRoleFlag::Required),
             0x02U},
            {"Pickup",
             NetEntityId::Compose(0x11223344U, 4U),
             0x55667788U,
             AnimSceneRoleKind::Pickup,
             0U,
             0x04U},
        }};
    const auto definitionFingerprint =
        ComputeAnimSceneDefinitionFingerprint(definition);
    CHECK(
        (definitionFingerprint.low | definitionFingerprint.high) != 0U);
    definition.fingerprintLow = definitionFingerprint.low;
    definition.fingerprintHigh = definitionFingerprint.high;
    const auto definitionBytes = EncodeAnimSceneDefinition(definition);
    CHECK(
        definitionBytes.size() <=
        kMaximumAnimSceneDefinitionPayloadSize);
    const auto decodedDefinition =
        DecodeAnimSceneDefinition(definitionBytes);
    CHECK(decodedDefinition.has_value());
    CHECK(*decodedDefinition == definition);
    auto definitionReserved = definitionBytes;
    definitionReserved[50] = 1U;
    CHECK(!DecodeAnimSceneDefinition(definitionReserved).has_value());
    auto definitionCreateReserved = definitionBytes;
    definitionCreateReserved[57] = 1U;
    CHECK(!DecodeAnimSceneDefinition(definitionCreateReserved).has_value());
    auto definitionWrongFingerprint = definitionBytes;
    definitionWrongFingerprint[24] ^= 0x01U;
    CHECK(!DecodeAnimSceneDefinition(definitionWrongFingerprint).has_value());

    auto zeroFingerprintDefinition = definition;
    zeroFingerprintDefinition.fingerprintLow = 0U;
    zeroFingerprintDefinition.fingerprintHigh = 0U;
    bool zeroFingerprintRejected{};
    try {
        (void)EncodeAnimSceneDefinition(zeroFingerprintDefinition);
    } catch (const std::invalid_argument&) {
        zeroFingerprintRejected = true;
    }
    CHECK(zeroFingerprintRejected);

    auto unsortedDefinition = definition;
    std::swap(
        unsortedDefinition.roles[0],
        unsortedDefinition.roles[1]);
    bool unsortedDefinitionRejected{};
    try {
        (void)ComputeAnimSceneDefinitionFingerprint(unsortedDefinition);
    } catch (const std::invalid_argument&) {
        unsortedDefinitionRejected = true;
    }
    CHECK(unsortedDefinitionRejected);

    auto duplicateEntityDefinition = definition;
    duplicateEntityDefinition.roles[1].entityId =
        duplicateEntityDefinition.roles[0].entityId;
    bool duplicateEntityRejected{};
    try {
        (void)ComputeAnimSceneDefinitionFingerprint(
            duplicateEntityDefinition);
    } catch (const std::invalid_argument&) {
        duplicateEntityRejected = true;
    }
    CHECK(duplicateEntityRejected);

    auto invalidTextDefinition = definition;
    invalidTextDefinition.resourceName = "script\nintro";
    bool invalidTextRejected{};
    try {
        (void)ComputeAnimSceneDefinitionFingerprint(invalidTextDefinition);
    } catch (const std::invalid_argument&) {
        invalidTextRejected = true;
    }
    CHECK(invalidTextRejected);

    auto oversizedResourceDefinition = definition;
    oversizedResourceDefinition.resourceName.assign(
        kMaximumAnimSceneResourceBytes + 1U,
        'A');
    bool oversizedResourceRejected{};
    try {
        (void)ComputeAnimSceneDefinitionFingerprint(
            oversizedResourceDefinition);
    } catch (const std::invalid_argument&) {
        oversizedResourceRejected = true;
    }
    CHECK(oversizedResourceRejected);

    auto oversizedPlaybackDefinition = definition;
    oversizedPlaybackDefinition.playbackList.assign(
        kMaximumAnimScenePlaybackListBytes + 1U,
        'P');
    bool oversizedPlaybackRejected{};
    try {
        (void)ComputeAnimSceneDefinitionFingerprint(
            oversizedPlaybackDefinition);
    } catch (const std::invalid_argument&) {
        oversizedPlaybackRejected = true;
    }
    CHECK(oversizedPlaybackRejected);

    auto oversizedRoleNameDefinition = definition;
    oversizedRoleNameDefinition.roles.front().roleName.assign(
        kMaximumAnimSceneRoleNameBytes + 1U,
        'R');
    bool oversizedRoleNameRejected{};
    try {
        (void)ComputeAnimSceneDefinitionFingerprint(
            oversizedRoleNameDefinition);
    } catch (const std::invalid_argument&) {
        oversizedRoleNameRejected = true;
    }
    CHECK(oversizedRoleNameRejected);

    auto oversizedDefinitionPayload = definitionBytes;
    oversizedDefinitionPayload.resize(
        kMaximumAnimSceneDefinitionPayloadSize + 1U);
    CHECK(
        !DecodeAnimSceneDefinition(oversizedDefinitionPayload)
             .has_value());

    auto invalidCreateOptionsDefinition = definition;
    invalidCreateOptionsDefinition.createOptionFlags = 0x04U;
    bool invalidCreateOptionsRejected{};
    try {
        (void)ComputeAnimSceneDefinitionFingerprint(
            invalidCreateOptionsDefinition);
    } catch (const std::invalid_argument&) {
        invalidCreateOptionsRejected = true;
    }
    CHECK(invalidCreateOptionsRejected);

    auto tooManyRolesDefinition = definition;
    tooManyRolesDefinition.roles.assign(
        kMaximumAnimSceneDefinitionRoles + 1U,
        definition.roles.front());
    bool tooManyRolesRejected{};
    try {
        (void)ComputeAnimSceneDefinitionFingerprint(
            tooManyRolesDefinition);
    } catch (const std::invalid_argument&) {
        tooManyRolesRejected = true;
    }
    CHECK(tooManyRolesRejected);

    const AnimSceneControlPayload playCommit{
        definition.hostEntityId,
        definition.missionEpoch,
        definition.cinematicGeneration,
        definition.definitionRevision,
        9U,
        definition.fingerprintLow,
        definition.fingerprintHigh,
        12'345U,
        0.25F,
        1.0F,
        AnimSceneControlKind::HostPlayCommit,
        static_cast<std::uint8_t>(PlayerSlot::Host),
        AnimSceneControlReason::None,
        static_cast<std::uint32_t>(
            AnimSceneControlFlag::LateJoin)};
    const auto playCommitBytes = EncodeAnimSceneControl(playCommit);
    CHECK(playCommitBytes.size() == kAnimSceneControlPayloadSize);
    CHECK(DecodeAnimSceneControl(playCommitBytes) == playCommit);
    auto controlReserved = playCommitBytes;
    controlReserved[59] = 1U;
    CHECK(!DecodeAnimSceneControl(controlReserved).has_value());
    auto invalidControl = playCommit;
    invalidControl.senderSlot =
        static_cast<std::uint8_t>(PlayerSlot::Guest);
    bool invalidControlRejected{};
    try {
        (void)EncodeAnimSceneControl(invalidControl);
    } catch (const std::invalid_argument&) {
        invalidControlRejected = true;
    }
    CHECK(invalidControlRejected);

    const EquipmentStatePayload equipment{
        source.entityId,
        0xAABBCCDDU,
        42U,
        static_cast<std::uint32_t>(
            EquipmentStateFlag::Equipped)};
    const auto equipmentBytes =
        EncodeEquipmentState(equipment);
    CHECK(
        equipmentBytes.size() ==
        kEquipmentStatePayloadSize);
    const auto decodedEquipment =
        DecodeEquipmentState(equipmentBytes);
    CHECK(decodedEquipment.has_value());
    CHECK(
        decodedEquipment->weaponHash ==
        0xAABBCCDDU);
    CHECK(decodedEquipment->ammo == 42U);
    auto equipmentWithReserved = equipmentBytes;
    equipmentWithReserved[20] = 1U;
    CHECK(
        !DecodeEquipmentState(
             equipmentWithReserved)
             .has_value());

    const PauseVotePayload pauseState{
        PauseVoteKind::AuthoritativeState,
        PlayerSlot::Host,
        static_cast<std::uint8_t>(
            PauseVoteFlag::GuestVoted) |
            static_cast<std::uint8_t>(
                PauseVoteFlag::Paused),
        42U};
    const auto pauseBytes =
        EncodePauseVote(pauseState);
    CHECK(pauseBytes.size() == kPauseVotePayloadSize);
    const auto decodedPause =
        DecodePauseVote(pauseBytes);
    CHECK(decodedPause.has_value());
    CHECK(
        decodedPause->kind ==
        PauseVoteKind::AuthoritativeState);
    CHECK(decodedPause->generation == 42U);
    auto pauseWithReserved = pauseBytes;
    pauseWithReserved[8] = 1U;
    CHECK(!DecodePauseVote(pauseWithReserved).has_value());

    const CommandPayload command{
        CommandOpcode::ApplyTransform,
        3U,
        source.entityId,
        {11.0F, 12.0F, 13.0F},
        45.0F,
        0.5F};
    const auto commandBytes = EncodeCommand(command);
    CHECK(commandBytes.size() == 32U);
    const auto decodedCommand = DecodeCommand(commandBytes);
    CHECK(decodedCommand.has_value());
    CHECK(decodedCommand->opcode == CommandOpcode::ApplyTransform);
    CHECK(decodedCommand->target == source.entityId);
    CHECK(decodedCommand->position.y == 12.0F);
    CHECK(decodedCommand->heading == 45.0F);
    const CommandPayload diagnosticMarker{
        CommandOpcode::DiagnosticMarker,
        0U,
        NetEntityId{0x0001'0000'0100'0001ULL},
        {1.0F, 2.0F, 3.0F},
        180.0F,
        1.0F};
    const auto decodedDiagnosticMarker =
        DecodeCommand(EncodeCommand(diagnosticMarker));
    CHECK(decodedDiagnosticMarker.has_value());
    CHECK(
        decodedDiagnosticMarker->opcode ==
        CommandOpcode::DiagnosticMarker);
    CHECK(decodedDiagnosticMarker->target == diagnosticMarker.target);

    const auto guestId = source.entityId;
    const auto hostId = NetEntityId::Compose(0x11223344U, 1U);
    const DownedStatePayload downed{
        guestId,
        PlayerLifecycle::Downed,
        0.0F};
    const auto downedBytes = EncodeDownedState(downed);
    CHECK(downedBytes.size() == 16U);
    CHECK(downedBytes[8] == 1U);
    CHECK(downedBytes[9] == 0U);
    CHECK(downedBytes[10] == 0U);
    CHECK(downedBytes[11] == 0U);
    const auto decodedDowned = DecodeDownedState(downedBytes);
    CHECK(decodedDowned.has_value());
    CHECK(decodedDowned->entityId == guestId);
    CHECK(
        decodedDowned->lifecycle ==
        PlayerLifecycle::Downed);

    const ReviveRequestPayload request{hostId, guestId};
    const auto requestBytes = EncodeReviveRequest(request);
    CHECK(requestBytes.size() == 16U);
    const auto decodedRequest = DecodeReviveRequest(requestBytes);
    CHECK(decodedRequest.has_value());
    CHECK(decodedRequest->reviverId == hostId);
    CHECK(decodedRequest->targetId == guestId);

    const ReviveCompletePayload complete{hostId, guestId, 0.35F};
    const auto completeBytes = EncodeReviveComplete(complete);
    CHECK(completeBytes.size() == 20U);
    const auto decodedComplete = DecodeReviveComplete(completeBytes);
    CHECK(decodedComplete.has_value());
    CHECK(decodedComplete->reviverId == hostId);
    CHECK(decodedComplete->targetId == guestId);
    CHECK(
        std::abs(decodedComplete->healthFraction - 0.35F) <
        0.0001F);
}

void AnimationReplicationPayloadContracts() {
    CHECK(kProtocolVersion == 27U);
    CHECK(
        static_cast<std::uint16_t>(MessageType::PlayerAnimationState) ==
        28U);
    CHECK(
        static_cast<std::uint16_t>(MessageType::MotionReplicationConfig) ==
        29U);
    CHECK(
        static_cast<std::uint16_t>(MessageType::PlayerAction) ==
        30U);
    CHECK(
        static_cast<std::uint16_t>(MessageType::MissionCameraState) ==
        31U);
    CHECK(
        static_cast<std::uint16_t>(MessageType::MissionCinematicState) ==
        35U);
    CHECK(
        static_cast<std::uint16_t>(MessageType::MissionCinematicAction) ==
        36U);
    CHECK(
        static_cast<std::uint16_t>(MessageType::PlayerAppearanceState) ==
        37U);
    CHECK(
        static_cast<std::uint16_t>(MessageType::AnimSceneReplicaState) ==
        38U);
    CHECK(
        static_cast<std::uint16_t>(MessageType::AnimSceneDefinition) ==
        39U);
    CHECK(
        static_cast<std::uint16_t>(MessageType::AnimSceneControl) ==
        40U);

    constexpr auto capabilities =
        static_cast<std::uint32_t>(
            PlayerAnimationCapability::GraphIdentifier) |
        static_cast<std::uint32_t>(
            PlayerAnimationCapability::StateIdentifier) |
        static_cast<std::uint32_t>(
            PlayerAnimationCapability::ClipIdentifiers) |
        static_cast<std::uint32_t>(
            PlayerAnimationCapability::NormalizedPhase) |
        static_cast<std::uint32_t>(
            PlayerAnimationCapability::PlaybackRate) |
        static_cast<std::uint32_t>(
            PlayerAnimationCapability::BlendWeights) |
        static_cast<std::uint32_t>(
            PlayerAnimationCapability::TransitionProgress) |
        static_cast<std::uint32_t>(
            PlayerAnimationCapability::RuntimeFlags);
    constexpr auto flags =
        static_cast<std::uint32_t>(
            PlayerAnimationStateFlag::GraphHashValid) |
        static_cast<std::uint32_t>(
            PlayerAnimationStateFlag::StateHashValid) |
        static_cast<std::uint32_t>(
            PlayerAnimationStateFlag::PrimaryClipHashValid) |
        static_cast<std::uint32_t>(
            PlayerAnimationStateFlag::SecondaryClipHashValid) |
        static_cast<std::uint32_t>(
            PlayerAnimationStateFlag::PrimaryPhaseValid) |
        static_cast<std::uint32_t>(
            PlayerAnimationStateFlag::SecondaryPhaseValid) |
        static_cast<std::uint32_t>(
            PlayerAnimationStateFlag::PrimaryPlaybackRateValid) |
        static_cast<std::uint32_t>(
            PlayerAnimationStateFlag::SecondaryPlaybackRateValid) |
        static_cast<std::uint32_t>(
            PlayerAnimationStateFlag::PrimaryBlendWeightValid) |
        static_cast<std::uint32_t>(
            PlayerAnimationStateFlag::SecondaryBlendWeightValid) |
        static_cast<std::uint32_t>(
            PlayerAnimationStateFlag::TransitionProgressValid) |
        static_cast<std::uint32_t>(
            PlayerAnimationStateFlag::Transitioning) |
        static_cast<std::uint32_t>(
            PlayerAnimationStateFlag::RootMotionActive) |
        static_cast<std::uint32_t>(PlayerAnimationStateFlag::Looping);

    const PlayerAnimationStatePayload source{
        NetEntityId::Compose(0x11223344U, 7U),
        PlayerSlot::Guest,
        kPlayerAnimationStateSchemaVersion,
        PlayerAnimationSampleSource::VersionedMemoryReader,
        17U,
        0xA1B2C3D4U,
        capabilities,
        flags,
        0x01020304U,
        0x11121314U,
        0x21222324U,
        0x31323334U,
        0.25F,
        0.75F,
        1.1F,
        -0.5F,
        0.6F,
        0.4F,
        0.35F};
    const auto bytes = EncodePlayerAnimationState(source);
    CHECK(bytes.size() == kPlayerAnimationStatePayloadSize);
    CHECK(
        bytes[68] == static_cast<std::uint8_t>(
                         PlayerAnimationSampleSource::VersionedMemoryReader));
    CHECK(bytes[69] == 0U && bytes[70] == 0U && bytes[71] == 0U);
    const auto decoded = DecodePlayerAnimationState(bytes);
    CHECK(decoded.has_value());
    CHECK(decoded->entityId == source.entityId);
    CHECK(decoded->slot == PlayerSlot::Guest);
    CHECK(decoded->locomotionEpoch == 17U);
    CHECK(decoded->sampleSequence == 0xA1B2C3D4U);
    CHECK(decoded->graphHash == source.graphHash);
    CHECK(decoded->primaryNormalizedPhase == 0.25F);
    CHECK(decoded->secondaryPlaybackRate == -0.5F);
    CHECK(decoded->source == PlayerAnimationSampleSource::VersionedMemoryReader);

    auto malformed = bytes;
    malformed[69] = 1U;
    CHECK(!DecodePlayerAnimationState(malformed).has_value());
    malformed = bytes;
    malformed[19] = 0x80U;  // Unknown capability bit.
    CHECK(!DecodePlayerAnimationState(malformed).has_value());
    malformed = bytes;
    malformed[68] = static_cast<std::uint8_t>(
        PlayerAnimationSampleSource::None);
    CHECK(!DecodePlayerAnimationState(malformed).has_value());
    CHECK(
        !DecodePlayerAnimationState(
             std::span<const std::uint8_t>{bytes.data(), bytes.size() - 1U})
             .has_value());

    auto invalidNumeric = source;
    invalidNumeric.primaryNormalizedPhase = 1.01F;
    bool invalidNumericRejected{};
    try {
        (void)EncodePlayerAnimationState(invalidNumeric);
    } catch (const std::invalid_argument&) {
        invalidNumericRejected = true;
    }
    CHECK(invalidNumericRejected);
    auto zeroSampleSequence = source;
    zeroSampleSequence.sampleSequence = 0U;
    bool zeroSampleSequenceRejected{};
    try {
        (void)EncodePlayerAnimationState(zeroSampleSequence);
    } catch (const std::invalid_argument&) {
        zeroSampleSequenceRejected = true;
    }
    CHECK(zeroSampleSequenceRejected);
    auto missingCapability = source;
    missingCapability.capabilities &=
        ~static_cast<std::uint32_t>(
            PlayerAnimationCapability::NormalizedPhase);
    bool missingCapabilityRejected{};
    try {
        (void)EncodePlayerAnimationState(missingCapability);
    } catch (const std::invalid_argument&) {
        missingCapabilityRejected = true;
    }
    CHECK(missingCapabilityRejected);

    auto emptyProbe = PlayerAnimationStatePayload{};
    emptyProbe.entityId = source.entityId;
    emptyProbe.slot = PlayerSlot::Host;
    emptyProbe.locomotionEpoch = 1U;
    emptyProbe.sampleSequence = 1U;
    CHECK(
        DecodePlayerAnimationState(
            EncodePlayerAnimationState(emptyProbe))
            .has_value());

    const MotionReplicationConfigPayload config{
        kMotionReplicationConfigSchemaVersion,
        MotionReplicationWireMode::AnimGraphReplica,
        static_cast<std::uint16_t>(
            MotionReplicationConfigFlag::AllowTaskNavmeshFallback) |
            static_cast<std::uint16_t>(
                MotionReplicationConfigFlag::EnableAnimSceneStoryVmProbe),
        3U};
    const auto configBytes = EncodeMotionReplicationConfig(config);
    CHECK(configBytes.size() == kMotionReplicationConfigPayloadSize);
    const auto decodedConfig = DecodeMotionReplicationConfig(configBytes);
    CHECK(decodedConfig.has_value());
    CHECK(decodedConfig->mode == MotionReplicationWireMode::AnimGraphReplica);
    CHECK(decodedConfig->revision == 3U);
    auto badConfig = configBytes;
    badConfig[1] = 0xFFU;
    CHECK(!DecodeMotionReplicationConfig(badConfig).has_value());
    badConfig = configBytes;
    badConfig[4] = 0U;
    badConfig[5] = 0U;
    badConfig[6] = 0U;
    badConfig[7] = 0U;
    CHECK(!DecodeMotionReplicationConfig(badConfig).has_value());
}

void SequenceAndEntityIds() {
    SequenceWindow window;
    CHECK(
        window.Observe(std::numeric_limits<std::uint32_t>::max() - 1U) ==
        SequenceDisposition::First);
    CHECK(
        window.Observe(std::numeric_limits<std::uint32_t>::max()) ==
        SequenceDisposition::Newer);
    CHECK(window.Observe(1U) == SequenceDisposition::Newer);
    CHECK(window.Observe(1U) == SequenceDisposition::Duplicate);
    CHECK(window.Observe(0U) == SequenceDisposition::Stale);

    NetEntityIdGenerator generator{0xAABBCCDDU};
    const auto first = generator.Next();
    const auto second = generator.Next();
    CHECK(first.Epoch() == 0xAABBCCDDU);
    CHECK(first.Counter() == 1U);
    CHECK(second.Counter() == 2U);

    EntityRegistry registry;
    CHECK(registry.Bind(first, 123));
    CHECK(registry.FindLocal(first) == 123);
    CHECK(registry.FindNetwork(123) == first);
    CHECK(!registry.Bind(second, 123));
    CHECK(registry.Remove(first));
    CHECK(registry.Size() == 0U);
    CHECK(registry.Bind(second, 456));
    const auto drained = registry.Drain();
    CHECK(drained.size() == 1U);
    CHECK(drained.front() == 456);
    CHECK(registry.Size() == 0U);
}

void WorldMirrorLifecycle() {
    WorldMirrorHost mirror{0xAABBCCDDU, 1'000U};
    const HostWorldEntitySample first{
        321,
        0x10203040U,
        WorldEntityKind::Ped,
        static_cast<std::uint8_t>(
            WorldEntityStateFlag::Human) |
            static_cast<std::uint8_t>(
                WorldEntityStateFlag::InCombat),
        WorldCombatTargetSlot::Host,
        {1.0F, 2.0F, 3.0F},
        {0.5F, 0.0F, 0.0F},
        90.0F,
        0.75F,
        0x55667788U};
    auto signals = mirror.Update(
        std::span<const HostWorldEntitySample>{&first, 1U},
        100U);
    CHECK(signals.size() == 1U);
    CHECK(signals.front().kind == WorldMirrorSignalKind::Spawn);
    const auto entityId = signals.front().state.entityId;
    CHECK(entityId.Counter() == 1'000U);
    CHECK(mirror.FindLocal(entityId) == 321);
    CHECK(mirror.FindNetwork(321) == entityId);
    CHECK(mirror.FindState(entityId)->weaponHash == first.weaponHash);

    const HostWorldEntitySample sceneObject{
        322,
        0x10203041U,
        WorldEntityKind::Object,
        static_cast<std::uint8_t>(
            WorldEntityStateFlag::ScriptOwned),
        WorldCombatTargetSlot::None,
        {2.0F, 3.0F, 4.0F},
        {},
        45.0F,
        1.0F,
        0U,
        WorldTaskKind::Cinematic,
        0,
        {2.0F, 3.0F, 4.0F},
        HostWorldEntityPriority::ScriptOwned,
        2.0F};
    WorldMirrorHost objectMirror{0xAABBCCDEU, 2'000U};
    auto objectSignals = objectMirror.Update(
        std::span<const HostWorldEntitySample>{&sceneObject, 1U},
        110U);
    CHECK(std::ranges::any_of(
        objectSignals,
        [](const auto& signal) {
            return signal.kind == WorldMirrorSignalKind::Spawn &&
                   signal.state.kind == WorldEntityKind::Object;
        }));
    CHECK(objectMirror.FindNetwork(322).has_value());

    auto moved = first;
    moved.position.x = 2.0F;
    signals = mirror.Update(
        std::span<const HostWorldEntitySample>{&moved, 1U},
        200U);
    CHECK(signals.size() == 1U);
    CHECK(signals.front().kind == WorldMirrorSignalKind::Update);
    CHECK(signals.front().state.entityId == entityId);
    CHECK(signals.front().state.position.x == 2.0F);

    signals = mirror.Update({}, 800U);
    CHECK(signals.empty());
    signals = mirror.Update({}, 950U);
    CHECK(signals.size() == 1U);
    CHECK(signals.front().kind == WorldMirrorSignalKind::Despawn);
    CHECK(signals.front().state.entityId == entityId);
    CHECK(!mirror.FindLocal(entityId).has_value());
    CHECK(!mirror.FindNetwork(321).has_value());

    const std::array mountedPair{
        HostWorldEntitySample{
            700,
            0xABCDEF01U,
            WorldEntityKind::Ped,
            0U,
            WorldCombatTargetSlot::None,
            {5.0F, 6.0F, 7.0F},
            {},
            15.0F,
            1.0F,
            0U,
            WorldTaskKind::Locomotion},
        HostWorldEntitySample{
            701,
            0xABCDEF02U,
            WorldEntityKind::Ped,
            static_cast<std::uint8_t>(
                WorldEntityStateFlag::Human) |
                static_cast<std::uint8_t>(
                    WorldEntityStateFlag::Mounted),
            WorldCombatTargetSlot::None,
            {5.0F, 6.0F, 8.0F},
            {},
            15.0F,
            1.0F,
            0U,
            WorldTaskKind::Mounted,
            700}};
    signals = mirror.Update(mountedPair, 1'000U);
    CHECK(signals.size() == 2U);
    const auto horse = std::find_if(
        signals.begin(),
        signals.end(),
        [](const WorldMirrorSignal& signal) {
            return signal.state.modelHash == 0xABCDEF01U;
        });
    const auto rider = std::find_if(
        signals.begin(),
        signals.end(),
        [](const WorldMirrorSignal& signal) {
            return signal.state.modelHash == 0xABCDEF02U;
        });
    CHECK(horse != signals.end());
    CHECK(rider != signals.end());
    CHECK(rider->state.parentEntityId == horse->state.entityId);
    CHECK(rider->state.taskKind == WorldTaskKind::Mounted);

    const auto stableHorseId = horse->state.entityId;
    const auto stableRiderId = rider->state.entityId;
    signals = mirror.ReplayStableSpawns();
    CHECK(signals.size() == 2U);
    CHECK(signals[0].kind == WorldMirrorSignalKind::Spawn);
    CHECK(signals[1].kind == WorldMirrorSignalKind::Spawn);
    CHECK(signals[0].state.entityId == stableHorseId);
    CHECK(signals[1].state.entityId == stableRiderId);
    CHECK(signals[1].state.parentEntityId == stableHorseId);
}

void WorldMirrorEntityGraphOrdering() {
    constexpr auto kHuman = static_cast<std::uint8_t>(
        WorldEntityStateFlag::Human);
    constexpr auto kHorse = static_cast<std::uint8_t>(
        WorldEntityStateFlag::Horse);
    constexpr auto kMounted = static_cast<std::uint8_t>(
        WorldEntityStateFlag::Mounted);
    WorldMirrorHost host{0x11223344U, 1'000U};
    const HostWorldEntitySample mount{
        800,
        0x10000001U,
        WorldEntityKind::Ped,
        kHorse,
        WorldCombatTargetSlot::None,
        {1.0F, 2.0F, 3.0F}};
    const HostWorldEntitySample rider{
        801,
        0x10000002U,
        WorldEntityKind::Ped,
        static_cast<std::uint8_t>(kHuman | kMounted),
        WorldCombatTargetSlot::None,
        {1.0F, 2.0F, 4.0F},
        {},
        0.0F,
        1.0F,
        0U,
        WorldTaskKind::Mounted,
        800};
    const std::array reversed{rider, mount};
    auto signals = host.Update(reversed, 100U);
    CHECK(signals.size() == 2U);
    CHECK(signals[0].state.modelHash == mount.modelHash);
    CHECK(signals[1].state.modelHash == rider.modelHash);
    CHECK(
        signals[1].state.parentEntityId ==
        signals[0].state.entityId);
    const auto oldMountId = signals[0].state.entityId;
    const auto oldRiderId = signals[1].state.entityId;

    auto replacementMount = mount;
    replacementMount.modelHash = 0x10000003U;
    const std::array replacementPair{rider, replacementMount};
    signals = host.Update(replacementPair, 200U);
    CHECK(signals.size() == 4U);
    CHECK(signals[0].kind == WorldMirrorSignalKind::Despawn);
    CHECK(signals[0].state.entityId == oldRiderId);
    CHECK(signals[1].kind == WorldMirrorSignalKind::Despawn);
    CHECK(signals[1].state.entityId == oldMountId);
    CHECK(signals[2].kind == WorldMirrorSignalKind::Spawn);
    CHECK(signals[2].state.modelHash == replacementMount.modelHash);
    CHECK(signals[3].kind == WorldMirrorSignalKind::Spawn);
    CHECK(signals[3].state.modelHash == rider.modelHash);
    CHECK(
        signals[3].state.parentEntityId ==
        signals[2].state.entityId);

    const auto parent = signals[2].state;
    const auto child = signals[3].state;
    WorldMirrorGuestGraph guest{4U, 8U};
    signals = guest.ApplyState(child, 10U);
    CHECK(signals.empty());
    CHECK(guest.Stats().nodeCount == 1U);
    CHECK(guest.Stats().pendingCount == 1U);

    signals = guest.ApplyState(parent, 11U);
    CHECK(signals.size() == 2U);
    CHECK(signals[0].kind == WorldMirrorSignalKind::Spawn);
    CHECK(signals[0].state.entityId == parent.entityId);
    CHECK(signals[1].kind == WorldMirrorSignalKind::Spawn);
    CHECK(signals[1].state.entityId == child.entityId);
    CHECK(guest.Stats().activeCount == 2U);

    signals = guest.ApplyDespawn(parent.entityId, 20U);
    CHECK(signals.size() == 2U);
    CHECK(signals[0].state.entityId == child.entityId);
    CHECK(signals[1].state.entityId == parent.entityId);
    CHECK(guest.Stats().nodeCount == 0U);
    CHECK(guest.Stats().cascadedDespawns == 1U);
    CHECK(guest.ApplyState(child, 19U).empty());
    CHECK(guest.Stats().staleMessages == 1U);
}

void WorldMirrorPriorityBudgetAndHysteresis() {
    const auto makeSample = [](
                                const LocalEntityHandle handle,
                                const float distance,
                                const HostWorldEntityPriority priority) {
        HostWorldEntitySample sample{};
        sample.localHandle = handle;
        sample.modelHash =
            0x20000000U + static_cast<std::uint32_t>(handle);
        sample.kind = WorldEntityKind::Ped;
        sample.flags = static_cast<std::uint8_t>(
            WorldEntityStateFlag::Human);
        sample.position = {distance, 0.0F, 0.0F};
        sample.heading = 0.0F;
        sample.healthFraction = 1.0F;
        sample.taskTarget = sample.position;
        sample.selectionPriority = priority;
        sample.selectionDistanceMeters = distance;
        return sample;
    };

    WorldMirrorHost bounded{0x22334455U, 1'000U, 48U};
    std::vector<HostWorldEntitySample> initial;
    initial.reserve(48U);
    for (LocalEntityHandle handle = 1; handle <= 48; ++handle) {
        initial.push_back(
            makeSample(
                handle,
                static_cast<float>(handle),
                HostWorldEntityPriority::Ambient));
    }
    auto signals = bounded.Update(initial, 100U);
    CHECK(signals.size() == 48U);
    CHECK(bounded.Size() == 48U);

    std::vector<HostWorldEntitySample> replacement;
    replacement.reserve(48U);
    for (LocalEntityHandle handle = 1; handle <= 47; ++handle) {
        replacement.push_back(
            makeSample(
                handle,
                static_cast<float>(handle),
                HostWorldEntityPriority::Ambient));
    }
    replacement.push_back(
        makeSample(
            100,
            75.0F,
            HostWorldEntityPriority::ScriptOwned));
    signals = bounded.Update(replacement, 200U);
    CHECK(bounded.Size() == 48U);
    CHECK(bounded.Stats().capacityEvictions == 1U);
    CHECK(
        std::count_if(
            signals.begin(),
            signals.end(),
            [](const WorldMirrorSignal& signal) {
                return signal.kind == WorldMirrorSignalKind::Despawn;
            }) == 1);
    CHECK(signals.front().kind == WorldMirrorSignalKind::Despawn);
    CHECK(
        std::any_of(
            signals.begin(),
            signals.end(),
            [](const WorldMirrorSignal& signal) {
                return signal.kind == WorldMirrorSignalKind::Spawn &&
                       signal.state.modelHash == 0x20000064U;
            }));

    WorldMirrorHost sticky{0x33445566U, 2'000U, 1U};
    auto incumbent = makeSample(
        201,
        30.0F,
        HostWorldEntityPriority::Ambient);
    signals = sticky.Update(
        std::span<const HostWorldEntitySample>{&incumbent, 1U},
        1'000U);
    CHECK(signals.size() == 1U);
    const auto incumbentId = signals.front().state.entityId;

    auto challenger = makeSample(
        202,
        20.0F,
        HostWorldEntityPriority::Ambient);
    const std::array closeCandidates{incumbent, challenger};
    signals = sticky.Update(closeCandidates, 1'100U);
    CHECK(signals.size() == 1U);
    CHECK(signals.front().kind == WorldMirrorSignalKind::Update);
    CHECK(signals.front().state.entityId == incumbentId);

    challenger.selectionDistanceMeters = 5.0F;
    challenger.position.x = 5.0F;
    challenger.taskTarget = challenger.position;
    const std::array decisiveCandidates{incumbent, challenger};
    signals = sticky.Update(decisiveCandidates, 4'100U);
    CHECK(signals.size() == 2U);
    CHECK(signals[0].kind == WorldMirrorSignalKind::Despawn);
    CHECK(signals[0].state.entityId == incumbentId);
    CHECK(signals[1].kind == WorldMirrorSignalKind::Spawn);
    CHECK(sticky.Size() == 1U);

    WorldMirrorHost missionRetention{0x44556677U, 3'000U, 4U};
    auto missionActor = makeSample(
        301,
        150.0F,
        HostWorldEntityPriority::ScriptOwned);
    signals = missionRetention.Update(
        std::span<const HostWorldEntitySample>{&missionActor, 1U},
        100U);
    CHECK(signals.size() == 1U);
    const auto missionActorId = signals.front().state.entityId;
    CHECK(missionRetention.Update({}, 15'099U).empty());
    signals = missionRetention.Update({}, 15'100U);
    CHECK(signals.size() == 1U);
    CHECK(signals.front().kind == WorldMirrorSignalKind::Despawn);
    CHECK(signals.front().state.entityId == missionActorId);
}

void ReviveStateMachine() {
    CoopPlayerStateMachine players;
    players.SetDowned(PlayerSlot::Guest);
    auto signals = players.Tick(
        3'999U,
        ReviveAttempt{
            PlayerSlot::Host,
            PlayerSlot::Guest,
            true,
            2.0F});
    CHECK(
        players.State(PlayerSlot::Guest).lifecycle ==
        PlayerLifecycle::Reviving);
    CHECK(
        players.State(PlayerSlot::Guest).reviveProgressMs ==
        3'999U);
    CHECK(
        signals.front().kind ==
        PlayerRuntimeSignalKind::ReviveStarted);

    signals = players.Tick(
        1U,
        ReviveAttempt{
            PlayerSlot::Host,
            PlayerSlot::Guest,
            true,
            2.0F});
    CHECK(
        players.State(PlayerSlot::Guest).lifecycle ==
        PlayerLifecycle::Alive);
    CHECK(
        std::abs(
            players.State(PlayerSlot::Guest).healthFraction -
            0.35F) <
        0.0001F);
    CHECK(
        signals.back().kind ==
        PlayerRuntimeSignalKind::ReviveCompleted);

    players.SetDowned(PlayerSlot::Guest);
    (void)players.Tick(
        1'000U,
        ReviveAttempt{
            PlayerSlot::Host,
            PlayerSlot::Guest,
            true,
            1.0F});
    signals = players.Tick(
        1U,
        ReviveAttempt{
            PlayerSlot::Host,
            PlayerSlot::Guest,
            true,
            2.01F});
    CHECK(
        players.State(PlayerSlot::Guest).lifecycle ==
        PlayerLifecycle::Downed);
    CHECK(
        signals.front().kind ==
        PlayerRuntimeSignalKind::ReviveCancelled);

    players.SetDowned(PlayerSlot::Host);
    signals = players.Tick(16U, std::nullopt);
    CHECK(signals.empty());
    CHECK(players.Tick(16U, std::nullopt).empty());
}

void BubbleBoundaries() {
    MissionBubbleController bubble;
    CHECK(
        bubble.Evaluate(false, 1'000.0F).zone ==
        MissionBubbleZone::Disabled);
    CHECK(
        bubble.Evaluate(true, 199.99F).zone ==
        MissionBubbleZone::Inside);
    CHECK(
        bubble.Evaluate(true, 200.0F).zone ==
        MissionBubbleZone::Warning);
    const auto exactLimit = bubble.Evaluate(true, 250.0F);
    CHECK(exactLimit.zone == MissionBubbleZone::Warning);
    CHECK(!exactLimit.executeOverLimitAction);
    const auto exceeded = bubble.Evaluate(true, 250.01F);
    CHECK(exceeded.zone == MissionBubbleZone::Exceeded);
    CHECK(exceeded.executeOverLimitAction);
    CHECK(!bubble.Evaluate(true, 300.0F).executeOverLimitAction);
    CHECK(
        bubble.Evaluate(
                  true,
                  std::numeric_limits<float>::quiet_NaN())
            .zone ==
        MissionBubbleZone::Exceeded);
}

void MenuEdges() {
    MenuController menu;
    CHECK(menu.Update(MenuInputState{.f9 = true}).visibilityChanged);
    CHECK(menu.IsOpen());
    (void)menu.Update({});
    (void)menu.Update(MenuInputState{.down = true});
    CHECK(menu.Selection() == 1U);
    (void)menu.Update({});
    const auto result =
        menu.Update(MenuInputState{.confirm = true});
    CHECK(result.command == BridgeCommand::TeleportToPlayer);
    (void)menu.Update({});
    CHECK(
        menu.Update(MenuInputState{.cancel = true}).visibilityChanged);
    CHECK(!menu.IsOpen());

    MenuController soloMenu;
    (void)soloMenu.Update(MenuInputState{.f9 = true});
    (void)soloMenu.Update({});
    (void)soloMenu.Update(MenuInputState{.up = true});
    CHECK(
        soloMenu.Commands()[soloMenu.Selection()] ==
        BridgeCommand::ArmFud1MissionProgression);
    CHECK(soloMenu.Commands().size() == 24U);
    CHECK(
        soloMenu.Commands()[0U] ==
        BridgeCommand::SkipCutscene);
    CHECK(
        soloMenu.Commands()[MenuController::PrimaryCommandCount()] ==
        BridgeCommand::RetryCheckpoint);

    MenuController columnMenu;
    (void)columnMenu.Update(MenuInputState{.f9 = true});
    (void)columnMenu.Update({});
    (void)columnMenu.Update(MenuInputState{.right = true});
    CHECK(
        columnMenu.Commands()[columnMenu.Selection()] ==
        BridgeCommand::RetryCheckpoint);
    (void)columnMenu.Update({});
    (void)columnMenu.Update(MenuInputState{.left = true});
    CHECK(columnMenu.Selection() == 0U);
    CHECK(
        MenuController::Label(BridgeCommand::StopSession) ==
        "Stop co-op session");
    CHECK(
        MenuController::Label(BridgeCommand::ToggleGhostRecord) ==
        "Ghost Record: start / stop");
    CHECK(
        MenuController::Label(BridgeCommand::ToggleGhostReplay) ==
        "Ghost Replay: start / stop");
    CHECK(
        MenuController::Label(BridgeCommand::GrantTestPistol) ==
        "Give pistol + max ammo (test)");
    CHECK(
        MenuController::Label(BridgeCommand::GrantTestLasso) ==
        "Give lasso (test)");
    CHECK(
        MenuController::Label(
            BridgeCommand::ProbeRepeatingShotgunShopUnlock) ==
        "Probe Repeating Shotgun shop unlock");
    CHECK(
        MenuController::Label(
            BridgeCommand::EnableRepeatingShotgunShopUnlock) ==
        "Enable Repeating Shotgun shop unlock (test)");
    CHECK(
        MenuController::Label(
            BridgeCommand::ProbePoisonThrowingKnifePamphlet) ==
        "Probe Poison Throwing Knife pamphlet");
    CHECK(
        MenuController::Label(BridgeCommand::SkipCutscene) ==
        "Vote: skip cutscene");
    CHECK(
        MenuController::Label(BridgeCommand::EmergencyRecover) ==
        "Emergency player recovery");
    CHECK(
        MenuController::Label(BridgeCommand::SaveProblemMarker) ==
        "Save problem marker");
}

void SessionOverlayAndPayloads() {
    SessionMenuController menu;
    CHECK(!menu.IsOpen());
    CHECK(menu.IsHudVisible());
    CHECK(
        menu.Update(MenuInputState{.f8 = true})
            .visibilityChanged);
    CHECK(menu.IsOpen());
    (void)menu.Update({});
    const auto stop =
        menu.Update(MenuInputState{.confirm = true});
    CHECK(stop.action == SessionOverlayAction::StopSession);
    (void)menu.Update({});
    CHECK(
        menu.Update(MenuInputState{.f10 = true})
            .hudVisibilityChanged);
    CHECK(!menu.IsHudVisible());
    (void)menu.Update({});
    CHECK(
        menu.Update(MenuInputState{.f8 = true})
            .visibilityChanged);
    CHECK(!menu.IsOpen());
    menu.MarkSessionReady(true, "ready");
    CHECK(menu.IsSessionReady());
    CHECK(menu.View().phase == SessionOverlayPhase::ReadyHost);
    CHECK(menu.View().actions.size() == 3U);
    CHECK(
        SessionMenuController::Label(
            SessionOverlayAction::StopSession) ==
        "STOP THE CURRENT CO-OP SESSION");
    menu.MarkSessionStopped("stopped");
    CHECK(!menu.IsSessionReady());
    CHECK(!menu.IsOpen());
    CHECK(menu.View().phase == SessionOverlayPhase::ChooseMode);

    const auto request = EncodeSessionMenuRequest(
        SessionMenuAction::JoinFromClipboard,
        "R2C1.example");
    CHECK(request.size() == 1U + std::string_view{"R2C1.example"}.size());
    CHECK(
        request.front() ==
        static_cast<std::uint8_t>(
            SessionMenuAction::JoinFromClipboard));
    const auto soloRequest = EncodeSessionMenuRequest(
        SessionMenuAction::ToggleSoloTest);
    CHECK(soloRequest.size() == 1U);
    CHECK(
        soloRequest.front() ==
        static_cast<std::uint8_t>(
            SessionMenuAction::ToggleSoloTest));
    const auto ghostRecordRequest = EncodeSessionMenuRequest(
        SessionMenuAction::ToggleGhostRecord);
    CHECK(ghostRecordRequest.size() == 1U);
    CHECK(
        ghostRecordRequest.front() ==
        static_cast<std::uint8_t>(
            SessionMenuAction::ToggleGhostRecord));
    const auto ghostReplayRequest = EncodeSessionMenuRequest(
        SessionMenuAction::ToggleGhostReplay);
    CHECK(ghostReplayRequest.size() == 1U);
    CHECK(
        ghostReplayRequest.front() ==
        static_cast<std::uint8_t>(
            SessionMenuAction::ToggleGhostReplay));
    const auto stopRequest = EncodeSessionMenuRequest(
        SessionMenuAction::StopSession);
    CHECK(stopRequest.size() == 1U);
    CHECK(
        stopRequest.front() ==
        static_cast<std::uint8_t>(
            SessionMenuAction::StopSession));

    const std::string message{"HOST ready"};
    const std::string invite{"R2C1.secret"};
    std::vector<std::uint8_t> status{
        static_cast<std::uint8_t>(
            SessionMenuStatusKind::ReadyHost),
        static_cast<std::uint8_t>(message.size()),
        0U,
        static_cast<std::uint8_t>(invite.size()),
        0U};
    status.insert(status.end(), message.begin(), message.end());
    status.insert(status.end(), invite.begin(), invite.end());
    const auto decoded = DecodeSessionMenuStatus(status);
    CHECK(decoded.has_value());
    CHECK(decoded->kind == SessionMenuStatusKind::ReadyHost);
    CHECK(decoded->message == message);
    CHECK(decoded->inviteCode == invite);
    status.push_back(0U);
    CHECK(!DecodeSessionMenuStatus(status).has_value());
}

void RemoteMotionPlanning() {
    const RemoteMotionInput closeInput{
        {0.0F, 0.0F, 100.0F},
        {0.0F, 0.0F, -0.25F},
        {1.0F, 0.0F, 100.0F},
        {0.5F, 0.0F, 0.0F},
        350.0F,
        10.0F,
        50U};
    const auto closeStep = PlanRemoteMotion(closeInput);
    CHECK(closeStep.mode == RemoteMotionMode::SmoothVelocity);
    CHECK(std::abs(closeStep.position.x - 0.060F) < 0.0001F);
    CHECK(std::abs(closeStep.velocity.x - 1.20F) < 0.0001F);
    CHECK(std::abs(closeStep.velocity.z) < 0.0001F);
    CHECK(std::abs(closeStep.heading - 314.0F) < 0.0001F);
    CHECK(closeStep.headingFollowsDestination);
    // A one-metre gap keeps the authoritative walking gait. V9.3 promoted
    // tiny errors to a run, producing the visible one-metre sprint/stop loop.
    CHECK(closeStep.locomotion == RemoteLocomotion::Walk);
    CHECK(closeStep.moveBlendRatio == 1.0F);
    CHECK(closeStep.taskSpeed == 1.0F);
    CHECK(closeStep.catchUpActive);
    CHECK(
        std::abs(closeStep.moveRateOverride - 1.0325F) <
        0.0001F);
    CHECK(
        std::abs(closeStep.taskDestination.x - 1.0F) <
        0.0001F);

    auto farInput = closeInput;
    farInput.targetPosition.x = kRemoteMotionSnapDistanceMeters;
    farInput.targetVelocity = {100.0F, 0.0F, 20.0F};
    const auto catchUpStep = PlanRemoteMotion(farInput);
    CHECK(
        catchUpStep.mode ==
        RemoteMotionMode::SmoothVelocity);
    CHECK(
        catchUpStep.position.x <
        kRemoteMotionSnapDistanceMeters);
    CHECK(std::abs(catchUpStep.position.x - 0.060F) < 0.0001F);
    CHECK(
        catchUpStep.moveRateOverride ==
        kRemoteMotionCatchUpMaximumMoveRate);
    CHECK(
        std::abs(
            catchUpStep.taskDestination.x -
            kRemoteMotionSnapDistanceMeters) <
        0.0001F);

    farInput.discontinuity = true;
    const auto farStep = PlanRemoteMotion(farInput);
    CHECK(farStep.mode == RemoteMotionMode::Snap);
    CHECK(farStep.position.x == kRemoteMotionSnapDistanceMeters);
    CHECK(
        std::abs(
            std::hypot(
                farStep.velocity.x,
                farStep.velocity.y) -
            kRemoteMotionMaximumHorizontalSpeed) <
        0.0001F);
    CHECK(farStep.velocity.z == 0.0F);
    CHECK(farStep.heading == 10.0F);
    CHECK(farStep.locomotion == RemoteLocomotion::Sprint);
    CHECK(farStep.moveBlendRatio == 3.0F);

    auto stoppedInput = closeInput;
    stoppedInput.targetPosition = stoppedInput.currentPosition;
    stoppedInput.targetVelocity = {};
    const auto stoppedStep = PlanRemoteMotion(stoppedInput);
    CHECK(stoppedStep.mode == RemoteMotionMode::SmoothVelocity);
    CHECK(stoppedStep.velocity.x == 0.0F);
    CHECK(stoppedStep.velocity.y == 0.0F);
    CHECK(stoppedStep.locomotion == RemoteLocomotion::Idle);
    CHECK(!stoppedStep.catchUpActive);
    CHECK(stoppedStep.moveRateOverride == 1.0F);
    CHECK(!stoppedStep.headingFollowsDestination);
    CHECK(std::abs(stoppedStep.heading - 8.0F) < 0.0001F);

    // Root direction follows the destination while catching up, even when
    // the replicated character/aim heading points the opposite way. This is
    // the stop-and-reverse conflict reproduced by the V9.2 solo log.
    auto directionConflictInput = stoppedInput;
    directionConflictInput.targetPosition = {0.0F, 10.0F, 100.0F};
    directionConflictInput.targetHeading = 270.0F;
    directionConflictInput.currentHeading = 0.0F;
    const auto directionConflictStep =
        PlanRemoteMotion(directionConflictInput);
    CHECK(directionConflictStep.headingFollowsDestination);
    CHECK(std::abs(directionConflictStep.heading - 0.0F) < 0.0001F);

    // Weapon actions may not own root steering while the ped is far from its
    // marker. The enter/exit gap is intentional hysteresis.
    CHECK(!ShouldSuppressRemoteAimRoot(false, false, false, 5.0F));
    CHECK(!ShouldSuppressRemoteAimRoot(false, true, true, 5.0F));
    CHECK(!ShouldSuppressRemoteAimRoot(false, true, false, 1.0F));
    CHECK(ShouldSuppressRemoteAimRoot(false, true, false, 1.25F));
    CHECK(ShouldSuppressRemoteAimRoot(true, true, false, 0.75F));
    CHECK(!ShouldSuppressRemoteAimRoot(true, true, false, 0.50F));

    // A stopped network target must still be reached on foot. Before V8.4,
    // authoritative zero velocity selected Idle and left a delayed proxy at
    // the wrong position indefinitely.
    auto delayedStoppedInput = stoppedInput;
    delayedStoppedInput.targetPosition.x = 5.0F;
    const auto delayedStoppedStep =
        PlanRemoteMotion(delayedStoppedInput);
    CHECK(delayedStoppedStep.catchUpActive);
    CHECK(
        delayedStoppedStep.locomotion ==
        RemoteLocomotion::Sprint);
    CHECK(delayedStoppedStep.taskDestination.x == 5.0F);
    CHECK(
        std::abs(delayedStoppedStep.velocity.x - 1.20F) <
        0.0001F);
    CHECK(
        std::abs(
            delayedStoppedStep.moveRateOverride -
            kRemoteMotionCatchUpMaximumMoveRate) <
        0.0001F);

    auto deadZoneInput = stoppedInput;
    deadZoneInput.targetPosition.x =
        kRemoteMotionCatchUpDeadZoneMeters - 0.01F;
    const auto deadZoneStep = PlanRemoteMotion(deadZoneInput);
    CHECK(!deadZoneStep.catchUpActive);
    CHECK(deadZoneStep.locomotion == RemoteLocomotion::Idle);

    auto walkingInput = stoppedInput;
    walkingInput.targetVelocity = {1.0F, 0.0F, 0.0F};
    CHECK(
        PlanRemoteMotion(walkingInput).locomotion ==
        RemoteLocomotion::Walk);
    walkingInput.targetVelocity = {3.0F, 0.0F, 0.0F};
    CHECK(
        PlanRemoteMotion(walkingInput).locomotion ==
        RemoteLocomotion::Run);
    walkingInput.targetVelocity = {6.0F, 0.0F, 0.0F};
    CHECK(
        PlanRemoteMotion(walkingInput).locomotion ==
        RemoteLocomotion::Sprint);

    auto dwellInput = stoppedInput;
    dwellInput.targetVelocity = {3.0F, 0.0F, 0.0F};
    dwellInput.currentLocomotion = RemoteLocomotion::Walk;
    dwellInput.locomotionInitialized = true;
    dwellInput.locomotionAgeMs =
        kRemoteMotionMinimumGaitDwellMs - 1U;
    const auto heldGait = PlanRemoteMotion(dwellInput);
    CHECK(heldGait.locomotion == RemoteLocomotion::Walk);
    CHECK(!heldGait.locomotionChanged);

    dwellInput.locomotionAgeMs =
        kRemoteMotionMinimumGaitDwellMs;
    const auto promotedGait = PlanRemoteMotion(dwellInput);
    CHECK(promotedGait.locomotion == RemoteLocomotion::Run);
    CHECK(promotedGait.locomotionChanged);
    CHECK(
        ShouldRefreshRemoteLocomotionTask(
            RemoteLocomotionTaskRefreshInput{
                false,
                true,
                RemoteLocomotion::Walk,
                promotedGait.locomotion,
                false,
                false}));
    CHECK(
        !ShouldRefreshRemoteLocomotionTask(
            RemoteLocomotionTaskRefreshInput{
                false,
                true,
                RemoteLocomotion::Run,
                promotedGait.locomotion,
                false,
                false}));
    // Reload invalidates task ownership. Even without a gait change, the next
    // transform must restore locomotion immediately after reload finishes.
    CHECK(
        ShouldRefreshRemoteLocomotionTask(
            RemoteLocomotionTaskRefreshInput{
                false,
                false,
                RemoteLocomotion::Run,
                RemoteLocomotion::Run,
                false,
                false}));

    dwellInput.currentLocomotion = RemoteLocomotion::Run;
    dwellInput.targetVelocity = {1.9F, 0.0F, 0.0F};
    CHECK(
        PlanRemoteMotion(dwellInput).locomotion ==
        RemoteLocomotion::Run);
    dwellInput.targetVelocity = {1.6F, 0.0F, 0.0F};
    CHECK(
        PlanRemoteMotion(dwellInput).locomotion ==
        RemoteLocomotion::Walk);

    dwellInput.currentLocomotion = RemoteLocomotion::Sprint;
    dwellInput.targetVelocity = {4.5F, 0.0F, 0.0F};
    CHECK(
        PlanRemoteMotion(dwellInput).locomotion ==
        RemoteLocomotion::Sprint);
    dwellInput.targetVelocity = {4.1F, 0.0F, 0.0F};
    CHECK(
        PlanRemoteMotion(dwellInput).locomotion ==
        RemoteLocomotion::Run);

    auto proportionalInput = stoppedInput;
    proportionalInput.targetPosition.x = 0.50F;
    const auto proportionalStep =
        PlanRemoteMotion(proportionalInput);
    CHECK(
        std::abs(proportionalStep.velocity.x - 1.20F) <
        0.0001F);

    CHECK(!ShouldApplyRemotePhysicsAssist(false, 1.49F));
    CHECK(ShouldApplyRemotePhysicsAssist(false, 1.50F));
    CHECK(ShouldApplyRemotePhysicsAssist(true, 0.75F));
    CHECK(!ShouldApplyRemotePhysicsAssist(true, 0.50F));

    CHECK(!IsRemoteNavigationStalledSample(
        2.99F,
        0.0F,
        1.0F,
        -1.0F));
    CHECK(IsRemoteNavigationStalledSample(
        5.0F,
        0.05F,
        0.50F,
        0.0F));
    CHECK(IsRemoteNavigationStalledSample(
        12.0F,
        0.50F,
        0.50F,
        0.10F));
    CHECK(!IsRemoteNavigationStalledSample(
        12.0F,
        0.50F,
        0.50F,
        0.50F));
    CHECK(!ShouldUseRemoteNavigationRecovery(
        false,
        8.0F,
        kRemoteNavigationStallEnterMs - 1U,
        false,
        false));
    CHECK(ShouldUseRemoteNavigationRecovery(
        false,
        8.0F,
        kRemoteNavigationStallEnterMs,
        false,
        false));
    CHECK(ShouldUseRemoteNavigationRecovery(
        true,
        1.26F,
        0U,
        false,
        false));
    CHECK(!ShouldUseRemoteNavigationRecovery(
        true,
        1.25F,
        0U,
        false,
        false));
    CHECK(ShouldUseRemoteNavigationRecovery(
        true,
        10.0F,
        2'000U,
        true,
        false));
    CHECK(!ShouldUseRemoteNavigationRecovery(
        false,
        10.0F,
        2'000U,
        true,
        false));
    CHECK(!ShouldUseDirectRemoteNavigationTarget(14.99F));
    CHECK(ShouldUseDirectRemoteNavigationTarget(15.0F));
    CHECK(ShouldRefreshRemoteNavigationDestination(
        false,
        0U,
        0.0F,
        false));
    CHECK(!ShouldRefreshRemoteNavigationDestination(
        true,
        kRemoteNavigationDestinationRefreshMs - 1U,
        0.0F,
        false));
    CHECK(ShouldRefreshRemoteNavigationDestination(
        true,
        kRemoteNavigationDestinationRefreshMs,
        0.0F,
        false));
    CHECK(!ShouldRefreshRemoteNavigationDestination(
        true,
        kRemoteNavigationUrgentRefreshMs - 1U,
        kRemoteNavigationDestinationDriftMeters,
        false));
    CHECK(ShouldRefreshRemoteNavigationDestination(
        true,
        kRemoteNavigationUrgentRefreshMs,
        kRemoteNavigationDestinationDriftMeters,
        false));
    CHECK(!HasRemoteNavigationRecoveryTimedOut(
        kRemoteNavigationMaximumActiveMs - 1U,
        false));
    CHECK(HasRemoteNavigationRecoveryTimedOut(
        kRemoteNavigationMaximumActiveMs,
        false));
    CHECK(!HasRemoteNavigationRecoveryTimedOut(
        kRemoteNavigationMaximumActiveMs,
        true));
    CHECK(ShouldApplyRemoteNavigationSafeRecovery(
        true,
        true,
        kRemoteNavigationSafeRecoveryMinimumErrorMeters,
        kRemoteNavigationSafeRecoveryMinimumDistanceMeters,
        false,
        false));
    CHECK(!ShouldApplyRemoteNavigationSafeRecovery(
        false,
        true,
        20.0F,
        2.0F,
        false,
        false));
    CHECK(!ShouldApplyRemoteNavigationSafeRecovery(
        true,
        false,
        20.0F,
        2.0F,
        false,
        false));
    CHECK(!ShouldApplyRemoteNavigationSafeRecovery(
        true,
        true,
        kRemoteNavigationSafeRecoveryMinimumErrorMeters - 0.01F,
        2.0F,
        false,
        false));
    CHECK(!ShouldApplyRemoteNavigationSafeRecovery(
        true,
        true,
        20.0F,
        kRemoteNavigationSafeRecoveryMaximumDistanceMeters + 0.01F,
        false,
        false));
    CHECK(!ShouldApplyRemoteNavigationSafeRecovery(
        true,
        true,
        20.0F,
        2.0F,
        true,
        false));
    CHECK(!ShouldApplyRemoteNavigationSafeRecovery(
        true,
        true,
        20.0F,
        2.0F,
        false,
        true));
    CHECK(!ShouldApplyRemoteHardResync(
        kRemoteMotionHardResyncDistanceMeters,
        kRemoteMotionHardResyncSustainMs - 1U,
        false,
        false,
        false,
        false));
    CHECK(ShouldApplyRemoteHardResync(
        kRemoteMotionHardResyncDistanceMeters,
        kRemoteMotionHardResyncSustainMs,
        false,
        false,
        false,
        false));
    CHECK(ShouldApplyRemoteHardResync(
        kRemoteMotionEmergencyHardResyncDistanceMeters,
        kRemoteMotionEmergencyHardResyncSustainMs,
        false,
        false,
        false,
        false));
    CHECK(!ShouldApplyRemoteHardResync(
        kRemoteMotionEmergencyHardResyncDistanceMeters,
        kRemoteMotionEmergencyHardResyncSustainMs,
        true,
        false,
        false,
        false));
    CHECK(!ShouldApplyRemoteHardResync(
        kRemoteMotionEmergencyHardResyncDistanceMeters,
        kRemoteMotionEmergencyHardResyncSustainMs,
        false,
        true,
        false,
        false));
    CHECK(!ShouldApplyRemoteHardResync(
        kRemoteMotionEmergencyHardResyncDistanceMeters,
        kRemoteMotionEmergencyHardResyncSustainMs,
        false,
        false,
        true,
        false));
    CHECK(!ShouldApplyRemoteHardResync(
        kRemoteMotionEmergencyHardResyncDistanceMeters,
        kRemoteMotionEmergencyHardResyncSustainMs,
        false,
        false,
        false,
        true));
    CHECK(ShouldExecuteDeferredRemoteTraversal(
        kRemoteTraversalActivationDistanceMeters,
        kRemoteTraversalMaximumAgeMs,
        false,
        false));
    CHECK(ShouldExecuteDeferredRemoteTraversal(
        1.0F,
        3'001U,
        false,
        false));
    CHECK(!ShouldExecuteDeferredRemoteTraversal(
        kRemoteTraversalActivationDistanceMeters + 0.01F,
        0U,
        false,
        false));
    CHECK(!ShouldExecuteDeferredRemoteTraversal(
        1.0F,
        kRemoteTraversalMaximumAgeMs + 1U,
        false,
        false));
    CHECK(!ShouldExecuteDeferredRemoteTraversal(
        1.0F,
        0U,
        true,
        false));

    auto verticalInput = stoppedInput;
    verticalInput.targetPosition.z = 90.0F;
    verticalInput.targetVelocity.z = -2.0F;
    const auto verticalStep = PlanRemoteMotion(verticalInput);
    CHECK(verticalStep.velocity.z < 0.0F);
    CHECK(
        verticalStep.velocity.z >=
        -kRemoteMotionMaximumVerticalSpeed);
    CHECK(verticalStep.position.z < verticalInput.currentPosition.z);

    // Reproduce the behaviour seen in the V9.0 game log: the native task
    // reports zero velocity again before every bridge tick. The facade keeps
    // the last command and feeds it back to the planner, so the ramp must
    // converge without a coordinate warp or overshooting the marker.
    auto resetByGameInput = stoppedInput;
    resetByGameInput.targetPosition.x = 10.0F;
    for (int tick = 0; tick < 80; ++tick) {
        const auto resetByGameStep =
            PlanRemoteMotion(resetByGameInput);
        CHECK(
            resetByGameStep.mode ==
            RemoteMotionMode::SmoothVelocity);
        CHECK(resetByGameStep.position.x <= 10.001F);
        resetByGameInput.currentPosition =
            resetByGameStep.position;
        resetByGameInput.currentVelocity =
            resetByGameStep.velocity;
    }
    CHECK(
        std::abs(
            resetByGameInput.targetPosition.x -
            resetByGameInput.currentPosition.x) <
        kRemoteMotionCatchUpDeadZoneMeters);

    // The same reset must not create a permanent trail behind a moving
    // sprint target. This models a short network delay while the render
    // marker itself remains smooth.
    auto movingResetInput = stoppedInput;
    movingResetInput.targetVelocity = {5.5F, 0.0F, 0.0F};
    for (int tick = 0; tick < 120; ++tick) {
        movingResetInput.targetPosition.x += 0.275F;
        const auto movingResetStep =
            PlanRemoteMotion(movingResetInput);
        movingResetInput.currentPosition =
            movingResetStep.position;
        movingResetInput.currentVelocity =
            movingResetStep.velocity;
    }
    CHECK(
        std::abs(
            movingResetInput.targetPosition.x -
            movingResetInput.currentPosition.x) <
        0.35F);

    auto invalidInput = closeInput;
    invalidInput.targetPosition.x =
        std::numeric_limits<float>::quiet_NaN();
    CHECK(
        PlanRemoteMotion(invalidInput).mode ==
        RemoteMotionMode::Hold);

    const Vec3 reported{1.0F, 2.0F, 101.5F};
    CHECK(
        SelectGroundSafePosition(reported, 100.0F).z ==
        100.0F);
    CHECK(
        SelectGroundSafePosition(reported, 90.0F).z ==
        reported.z);
    CHECK(
        SelectGroundSafePosition(
            reported,
            std::numeric_limits<float>::quiet_NaN())
            .z == reported.z);

    CHECK(
        ComputeRemoteRouteLookAheadMeters(
            RemoteLocomotion::Walk,
            0.0F,
            0.0F) == 0.75F);
    CHECK(
        ComputeRemoteRouteLookAheadMeters(
            RemoteLocomotion::Sprint,
            3.0F,
            100.0F) < 1.05F);
    CHECK(
        SelectPuppetControlMode(
            PlayerLocomotionMode::Aiming,
            false,
            false,
            false,
            false,
            false,
            false) == PuppetControlMode::AimingLocomotion);
    CHECK(
        SelectPuppetControlMode(
            PlayerLocomotionMode::Grounded,
            false,
            true,
            true,
            false,
            false,
            false) == PuppetControlMode::TraversalApproach);
    CHECK(
        SelectPuppetControlMode(
            PlayerLocomotionMode::Airborne,
            false,
            false,
            false,
            true,
            true,
            false) == PuppetControlMode::TraversalCommitted);

}

void RemoteSnapshotInterpolation() {
    RemoteSnapshotBuffer buffer;
    const auto entity =
        NetEntityId::Compose(0x11223344U, 2U);
    PlayerStatePayload first{
        entity,
        PlayerSlot::Guest,
        PlayerLifecycle::Alive,
        {0.0F, 0.0F, 100.0F},
        {10.0F, 0.0F, 0.0F},
        350.0F,
        1.0F,
        0U};
    first.locomotionEpoch = 1U;
    auto second = first;
    second.position = {0.5F, 0.0F, 100.0F};
    second.heading = 10.0F;
    auto third = second;
    third.position = {1.0F, 0.0F, 100.0F};
    third.heading = 30.0F;

    CHECK(buffer.Push(first, 1'000U, 10'000U));
    CHECK(buffer.Push(second, 1'050U, 10'050U));
    CHECK(buffer.Push(third, 1'100U, 10'100U));

    const auto interpolated = buffer.Sample(1'125U);
    CHECK(interpolated.has_value());
    CHECK(
        interpolated->mode ==
        RemoteSnapshotSampleMode::Interpolated);
    CHECK(
        std::abs(interpolated->state.position.x - 0.35F) <
        0.0001F);
    CHECK(
        std::abs(interpolated->state.heading - 4.0F) <
        0.0001F);
    CHECK(interpolated->senderTickMs == 10'035U);

    const auto extrapolated = buffer.Sample(1'250U);
    CHECK(extrapolated.has_value());
    CHECK(
        extrapolated->mode ==
        RemoteSnapshotSampleMode::Extrapolated);
    CHECK(
        std::abs(extrapolated->state.position.x - 1.6F) <
        0.0001F);
    CHECK(extrapolated->senderTickMs == 10'160U);

    const auto frozen = buffer.Sample(1'550U);
    CHECK(frozen.has_value());
    CHECK(
        frozen->mode ==
        RemoteSnapshotSampleMode::Frozen);
    CHECK(
        std::abs(frozen->state.position.x - 2.5F) <
        0.0001F);
    CHECK(frozen->state.velocity.x == 0.0F);

    RemoteSnapshotBuffer delayedBuffer;
    auto delayedFirst = first;
    delayedFirst.velocity = {};
    auto delayedSecond = delayedFirst;
    delayedSecond.position.x = 8.0F;
    CHECK(delayedBuffer.Push(delayedFirst, 2'000U));
    CHECK(delayedBuffer.Push(delayedSecond, 2'050U));
    const auto delayedSample = delayedBuffer.Sample(2'125U);
    CHECK(delayedSample.has_value());
    CHECK(
        delayedSample->mode ==
        RemoteSnapshotSampleMode::Interpolated);
    CHECK(
        std::abs(delayedSample->state.position.x - 6.272F) <
        0.001F);

    auto teleported = third;
    teleported.position = {200.0F, -50.0F, 110.0F};
    CHECK(buffer.Push(teleported, 1'150U));
    const auto teleportSample = buffer.Sample(1'150U);
    CHECK(teleportSample.has_value());
    CHECK(
        teleportSample->mode ==
        RemoteSnapshotSampleMode::Hold);
    CHECK(teleportSample->state.position.x == 200.0F);
    CHECK(teleportSample->state.position.y == -50.0F);

    auto invalid = second;
    invalid.position.x =
        std::numeric_limits<float>::quiet_NaN();
    CHECK(!buffer.Push(invalid, 1'550U));
    CHECK(!buffer.Push(second, 1'099U));

    auto afterGap = second;
    afterGap.position.x = 20.0F;
    CHECK(buffer.Push(afterGap, 2'150U));
    const auto resetSample = buffer.Sample(2'150U);
    CHECK(resetSample.has_value());
    CHECK(resetSample->mode == RemoteSnapshotSampleMode::Hold);
    CHECK(resetSample->state.position.x == 20.0F);

    buffer.Reset();
    CHECK(!buffer.Sample(2'200U).has_value());

    RemoteSnapshotBuffer jittered;
    CHECK(jittered.Push(first, 3'000U, 20'000U));
    CHECK(jittered.Push(second, 3'090U, 20'050U));
    CHECK(jittered.Push(third, 3'105U, 20'100U));
    CHECK(jittered.ArrivalJitterMs() > 0.0F);
    CHECK(
        jittered.InterpolationDelayMs() >
        kRemoteSnapshotBaseInterpolationDelayMs);

    auto nextEpoch = third;
    nextEpoch.locomotionEpoch = 2U;
    nextEpoch.position.x = 2.0F;
    CHECK(jittered.Push(nextEpoch, 3'150U, 20'150U));
    const auto epochSample = jittered.Sample(3'150U);
    CHECK(epochSample.has_value());
    CHECK(epochSample->mode == RemoteSnapshotSampleMode::Hold);
    CHECK(epochSample->state.position.x == 2.0F);

    CHECK(IsRemoteAnimationStateFresh(
        1'000U,
        10'000U,
        10'050U,
        1'500U));
    CHECK(!IsRemoteAnimationStateFresh(
        1'000U,
        10'000U,
        10'050U,
        1'501U));
    CHECK(!IsRemoteAnimationStateFresh(
        1'000U,
        10'000U,
        10'501U,
        1'100U));
    CHECK(IsRemoteAnimationStateFresh(
        1'000U,
        0U,
        0U,
        1'100U));
    CHECK(ShouldApplyAnimGraphDirectRootCorrection(false, false));
    CHECK(!ShouldApplyAnimGraphDirectRootCorrection(true, false));
    CHECK(!ShouldApplyAnimGraphDirectRootCorrection(false, true));
    CHECK(!ShouldApplyDirectReplicaPhysicalRootLeash(
        false,
        false,
        kDirectReplicaPhysicalRootLeashMeters + 1.0F));
    CHECK(!ShouldApplyDirectReplicaPhysicalRootLeash(
        true,
        true,
        kDirectReplicaPhysicalRootLeashMeters + 1.0F));
    CHECK(!ShouldApplyDirectReplicaPhysicalRootLeash(
        false,
        true,
        kDirectReplicaPhysicalRootLeashMeters - 0.01F));
    CHECK(ShouldApplyDirectReplicaPhysicalRootLeash(
        false,
        true,
        kDirectReplicaPhysicalRootLeashMeters));

    CHECK(
        SelectDirectReplicaVisualLocomotion(
            RemoteLocomotion::Sprint,
            0.0F) == RemoteLocomotion::Sprint);
    CHECK(
        SelectDirectReplicaVisualLocomotion(
            std::nullopt,
            0.0F) == RemoteLocomotion::Idle);
    CHECK(
        SelectDirectReplicaVisualLocomotion(
            std::nullopt,
            1.0F) == RemoteLocomotion::Walk);
    CHECK(
        SelectDirectReplicaVisualLocomotion(
            std::nullopt,
            2.0F) == RemoteLocomotion::Run);
    CHECK(
        SelectDirectReplicaVisualLocomotion(
            std::nullopt,
            3.0F) == RemoteLocomotion::Sprint);

    const auto visualDestination =
        ComputeDirectReplicaVisualTaskDestination(
            {10.0F, 20.0F, 30.0F},
            {3.0F, 4.0F, 0.0F},
            180.0F,
            RemoteLocomotion::Run);
    CHECK(std::abs(visualDestination.x - 18.4F) < 0.001F);
    CHECK(std::abs(visualDestination.y - 31.2F) < 0.001F);
    CHECK(visualDestination.z == 30.0F);
    const auto headingDestination =
        ComputeDirectReplicaVisualTaskDestination(
            {1.0F, 2.0F, 3.0F},
            {},
            0.0F,
            RemoteLocomotion::Sprint);
    CHECK(std::abs(headingDestination.x - 1.0F) < 0.001F);
    CHECK(std::abs(headingDestination.y - 22.0F) < 0.001F);
    CHECK(
        DirectReplicaVisualTaskSpeed(RemoteLocomotion::Walk) ==
        1.0F);
    CHECK(
        DirectReplicaVisualTaskSpeed(RemoteLocomotion::Sprint) ==
        3.0F);

    // RDR2 heading 0 faces +Y; 270 faces +X. This is intentionally
    // different from mathematical atan2(y, x).
    CHECK(std::abs(MovementHeadingFromVelocity(
                       {0.0F, 3.0F, 0.0F},
                       123.0F) -
                   0.0F) < 0.001F);
    CHECK(std::abs(MovementHeadingFromVelocity(
                       {3.0F, 0.0F, 0.0F},
                       123.0F) -
                   270.0F) < 0.001F);
    CHECK(MovementHeadingFromVelocity({}, 123.0F) == 123.0F);
    const auto northFacingVelocity = ComputePedRelativeVelocity(
        {2.0F, 5.0F, 0.0F},
        0.0F);
    CHECK(std::abs(northFacingVelocity.forward - 5.0F) < 0.001F);
    CHECK(std::abs(northFacingVelocity.right - 2.0F) < 0.001F);
    const auto westFacingVelocity = ComputePedRelativeVelocity(
        {-4.0F, 1.0F, 0.0F},
        90.0F);
    CHECK(std::abs(westFacingVelocity.forward - 4.0F) < 0.001F);
    CHECK(std::abs(westFacingVelocity.right - 1.0F) < 0.001F);
    CHECK(ClassifyRemoteMovementDirection(2.0F, 0.0F) ==
          RemoteMovementDirection::Forward);
    CHECK(ClassifyRemoteMovementDirection(-2.0F, 0.0F) ==
          RemoteMovementDirection::Backward);
    CHECK(ClassifyRemoteMovementDirection(0.0F, 2.0F) ==
          RemoteMovementDirection::Right);
    CHECK(ClassifyRemoteMovementDirection(0.0F, -2.0F) ==
          RemoteMovementDirection::Left);
    CHECK(ClassifyRemoteMovementDirection(1.0F, 1.0F) ==
          RemoteMovementDirection::ForwardRight);
    CHECK(ClassifyRemoteMovementDirection(0.05F, 0.05F) ==
          RemoteMovementDirection::None);

    CHECK(ShouldRefreshDirectReplicaVisualTask(
        {false,
         RemoteLocomotion::Idle,
         RemoteLocomotion::Walk,
         0U,
         0.0F}));
    CHECK(ShouldRefreshDirectReplicaVisualTask(
        {true,
         RemoteLocomotion::Walk,
         RemoteLocomotion::Run,
         100U,
         0.0F}));
    CHECK(!ShouldRefreshDirectReplicaVisualTask(
        {true,
         RemoteLocomotion::Run,
         RemoteLocomotion::Run,
         200U,
         90.0F}));
    CHECK(ShouldRefreshDirectReplicaVisualTask(
        {true,
         RemoteLocomotion::Run,
         RemoteLocomotion::Run,
         300U,
         20.0F}));
    CHECK(ShouldRefreshDirectReplicaVisualTask(
        {true,
         RemoteLocomotion::Idle,
         RemoteLocomotion::Idle,
         kDirectReplicaVisualTaskRefreshMs,
         0.0F}));
    CHECK(ShouldRefreshDirectReplicaVisualTask(
        {true,
         RemoteLocomotion::Walk,
         RemoteLocomotion::Walk,
         kDirectReplicaVisualTaskMinimumRefreshMs,
         0.0F,
         RemoteMovementDirection::Forward,
         RemoteMovementDirection::Backward}));
    CHECK(ShouldRefreshDirectReplicaVisualTask(
        {true,
         RemoteLocomotion::Idle,
         RemoteLocomotion::Idle,
         kDirectReplicaVisualTaskMinimumRefreshMs,
         kDirectReplicaTurnInPlaceHeadingDegrees,
         RemoteMovementDirection::None,
         RemoteMovementDirection::None}));

    CHECK(ShouldStartDirectReplicaTraversal(
        {100U, 0.5F, false, false, false, false}));
    CHECK(ShouldStartDirectReplicaTraversal(
        {100U, 5.0F, true, false, false, false}));
    CHECK(!ShouldStartDirectReplicaTraversal(
        {kDirectReplicaTraversalMaximumAgeMs + 1U,
         0.0F,
         true,
         false,
         false,
         false}));
    CHECK(!ShouldStartDirectReplicaTraversal(
        {100U, 0.0F, true, true, false, false}));
    CHECK(!ShouldStartDirectReplicaTraversal(
        {100U, 0.0F, true, false, true, false}));
    CHECK(!ShouldStartDirectReplicaTraversal(
        {100U, 0.0F, true, false, false, true}));
    CHECK(ShouldLaunchDirectReplicaAirborne(
        PlayerLocomotionMode::Grounded,
        PlayerLocomotionMode::Airborne,
        false,
        false));
    CHECK(!ShouldLaunchDirectReplicaAirborne(
        PlayerLocomotionMode::Airborne,
        PlayerLocomotionMode::Airborne,
        false,
        false));
    CHECK(!ShouldLaunchDirectReplicaAirborne(
        PlayerLocomotionMode::Grounded,
        PlayerLocomotionMode::Airborne,
        true,
        false));
}

void GateAndTelemetry() {
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    CHECK(
        VersionGate::Evaluate(
            supported,
            RuntimeMode{true, true, false})
            .allowed);
    CHECK(
        VersionGate::Evaluate(
            supported,
            RuntimeMode{true, false, true})
            .failure ==
        GateFailure::OnlineSessionDetected);
    auto badHash = supported;
    badHash.sha256[0] = '0';
    CHECK(
        VersionGate::Evaluate(
            badHash,
            RuntimeMode{true, true, false})
            .failure ==
        GateFailure::UnsupportedHash);

    TelemetryScheduler telemetry;
    CHECK(telemetry.ShouldEmit(0U));
    CHECK(!telemetry.ShouldEmit(49U));
    CHECK(telemetry.ShouldEmit(50U));
    CHECK(telemetry.ShouldEmit(100U));
    CHECK(!telemetry.ShouldEmit(101U));
    CHECK(telemetry.ShouldEmit(0U));
}

class TestFacade final : public IScriptHookFacade {
public:
    std::uint64_t TickMilliseconds() noexcept override {
        const auto current = tick;
        tick += tickReadAdvance;
        return current;
    }
    RuntimeMode QueryRuntimeMode() noexcept override { return mode; }
    std::optional<LocalPlayerSample> SampleLocalPlayer() noexcept override {
        return sampleAvailable
                   ? std::optional<LocalPlayerSample>{sample}
                   : std::nullopt;
    }
    std::optional<PlayerAppearanceStatePayload> SampleLocalAppearance(
        const NetEntityId entityId,
        const PlayerSlot slot,
        const std::uint32_t revision) noexcept override {
        if (!sampledAppearance.has_value()) {
            return std::nullopt;
        }
        auto result = *sampledAppearance;
        result.entityId = entityId;
        result.slot = slot;
        result.revision = revision;
        return result;
    }
    std::optional<MissionCameraSample>
    SampleMissionCamera() noexcept override {
        return sampledMissionCamera;
    }
    std::optional<AnimSceneReplicaStatePayload>
    SampleHostAnimScene(
        const NetEntityId hostEntityId,
        const std::uint32_t missionEpoch,
        const std::uint32_t cinematicGeneration,
        const std::uint32_t revision) noexcept override {
        if (!sampledAnimScene.has_value()) {
            return std::nullopt;
        }
        auto result = *sampledAnimScene;
        result.hostEntityId = hostEntityId;
        result.missionEpoch = missionEpoch;
        result.cinematicGeneration = cinematicGeneration;
        result.revision = revision;
        return result;
    }
    std::optional<LocalEntityHandle>
    SampledHostAnimSceneLocalHandle() noexcept override {
        return sampledAnimSceneLocalHandle;
    }
    std::vector<CapturedAnimSceneDefinition>
    DrainCapturedAnimSceneDefinitions() noexcept override {
        auto result = std::move(capturedAnimSceneDefinitions);
        capturedAnimSceneDefinitions.clear();
        return result;
    }
    std::optional<NetEntityId> FindKnownReplicaNetworkId(
        const LocalEntityHandle localHandle) noexcept override {
        const auto iterator = knownReplicaNetworkIds.find(localHandle);
        return iterator == knownReplicaNetworkIds.end()
                   ? std::nullopt
                   : std::optional<NetEntityId>{iterator->second};
    }
    RuntimeDivergenceDiagnostics
    SampleRuntimeDivergenceDiagnostics() noexcept override {
        return runtimeDivergenceDiagnostics;
    }
    std::optional<PlayerAnimationStatePayload>
    SampleLocalAnimationState(
        const NetEntityId entityId,
        const PlayerSlot slot,
        const std::uint16_t locomotionEpoch,
        const std::uint32_t sampleSequence) noexcept override {
        if (!sampledAnimation.has_value()) {
            return std::nullopt;
        }
        auto result = *sampledAnimation;
        result.entityId = entityId;
        result.slot = slot;
        result.locomotionEpoch = locomotionEpoch;
        result.sampleSequence = sampleSequence;
        return result;
    }
    std::optional<WorldStatePayload> SampleWorldState() noexcept override {
        return sampledWorld;
    }
    std::vector<HostWorldEntitySample> SampleWorldEntities(
        float,
        const std::size_t maximumEntities) noexcept override {
        auto result = sampledWorldEntities;
        if (result.size() > maximumEntities) {
            result.resize(maximumEntities);
        }
        return result;
    }
    std::optional<DamageIntentPayload> SampleWorldDamageIntent(
        const NetEntityId attackerId) noexcept override {
        auto result = pendingWorldDamageIntent;
        pendingWorldDamageIntent.reset();
        if (result.has_value()) {
            result->attackerId = attackerId;
        }
        return result;
    }
    std::vector<CampaignCapabilityObservation>
    DrainCampaignCapabilityObservations() noexcept override {
        auto result = std::move(pendingCampaignCapabilityObservations);
        pendingCampaignCapabilityObservations.clear();
        return result;
    }
    std::optional<CampaignMissionProbe> ProbeCampaignMission(
        const std::uint32_t expectedMissionId) noexcept override {
        if (!sampledCampaignMission.has_value() ||
            sampledCampaignMission->missionId != expectedMissionId) {
            return std::nullopt;
        }
        return sampledCampaignMission;
    }
    bool ApplyCampaignMissionCompletion(
        const std::uint32_t missionId,
        const std::uint64_t completionEventId,
        const std::uint8_t completionRating) noexcept override {
        ++campaignMissionCompletionApplyCount;
        appliedCampaignMissionId = missionId;
        appliedCampaignMissionEventId = completionEventId;
        appliedCampaignMissionRating = completionRating;
        return applyCampaignMissionCompletionResult;
    }
    std::optional<std::int32_t> QueryLocalCashBalance() noexcept override {
        return localCashBalance;
    }
    bool ApplyCampaignMissionCashAward(
        const std::uint64_t completionEventId,
        const std::int32_t amount) noexcept override {
        ++campaignMissionCashApplyCount;
        appliedCampaignMissionCashEventId = completionEventId;
        appliedCampaignMissionCashAmount = amount;
        return applyCampaignMissionCashAwardResult;
    }
    std::optional<float> HostGuestDistanceMeters() noexcept override {
        return distance;
    }
    MenuInputState ReadMenuInput() noexcept override { return menuInput; }
    void DrawMenu(
        bool,
        std::span<const BridgeCommand>,
        std::size_t) noexcept override {}
    void DrawSessionMenu(
        const SessionOverlayView& state) noexcept override {
        sessionMenuOpen = state.open;
        sessionMenuPhase = state.phase;
        sessionMenuStatus.assign(state.status);
        sessionMenuActionCount = state.actions.size();
        sessionMenuFirstAction = state.actions.empty()
            ? std::nullopt
            : std::optional{state.actions.front()};
    }
    void DrawBridgeHud(const BridgeHudState& state) noexcept override {
        try {
            hudStates.push_back(state);
        } catch (...) {
        }
    }
    void DrawPauseVoteStatus(
        const PauseVoteView& state) noexcept override {
        try {
            pauseVoteStates.push_back(state);
        } catch (...) {
        }
    }
    void DrawNotification(
        const std::string_view text,
        bool) noexcept override {
        try {
            notifications.emplace_back(text);
        } catch (...) {
        }
    }
    std::string ReadClipboardText() noexcept override {
        return clipboard;
    }
    bool WriteClipboardText(
        const std::string_view text) noexcept override {
        clipboard.assign(text);
        return true;
    }
    void ShowMissionBubbleWarning(float) noexcept override {}
    bool ExecuteCommand(BridgeCommand command) noexcept override {
        try {
            localCommands.push_back(command);
            return true;
        } catch (...) {
            return false;
        }
    }
    bool ApplyNetworkCommand(
        const CommandPayload& command) noexcept override {
        try {
            networkCommands.push_back(command);
            return true;
        } catch (...) {
            return false;
        }
    }
    bool ApplyRemoteTransform(
        const PlayerStatePayload& state) noexcept override {
        try {
            remoteTransforms.push_back(state);
            return applyRemoteTransformSucceeds;
        } catch (...) {
            return false;
        }
    }
    bool ApplyRemoteAnimationState(
        const PlayerAnimationStatePayload& state) noexcept override {
        try {
            remoteAnimationStates.push_back(state);
            return true;
        } catch (...) {
            return false;
        }
    }
    void ConfigureMotionReplication(
        const MotionReplicationConfigPayload& config) noexcept override {
        try {
            motionReplicationConfigs.push_back(config);
        } catch (...) {
        }
    }
    void SetAnimSceneCaptureAuthority(
        const bool hostAuthority) noexcept override {
        animSceneCaptureHostAuthority = hostAuthority;
        ++animSceneCaptureAuthorityChanges;
    }
    bool ApplyRemoteTraversal(
        const PlayerTraversalPayload& traversal) noexcept override {
        try {
            remoteTraversals.push_back(traversal);
            return true;
        } catch (...) {
            return false;
        }
    }
    bool ApplyRemotePlayerAction(
        const PlayerActionPayload& action) noexcept override {
        try {
            remotePlayerActions.push_back(action);
            return true;
        } catch (...) {
            return false;
        }
    }
    bool ApplyRestraintState(
        const RestraintStatePayload& state,
        const NetEntityId localEntityId) noexcept override {
        try {
            appliedRestraintStates.push_back(state);
            restraintLocalEntityIds.push_back(localEntityId);
            return true;
        } catch (...) {
            return false;
        }
    }
    bool ApplyRemoteIdentity(
        const PlayerIdentityPayload& identity) noexcept override {
        try {
            remoteIdentities.push_back(identity);
            return true;
        } catch (...) {
            return false;
        }
    }
    bool ApplyWorldState(
        const WorldStatePayload& state) noexcept override {
        appliedWorldStates.push_back(state);
        return true;
    }
    bool ApplyRemoteEquipment(
        const EquipmentStatePayload& state) noexcept override {
        remoteEquipment.push_back(state);
        return true;
    }
    bool UnlockLocalWeaponEntitlement(
        const std::uint32_t weaponHash) noexcept override {
        weaponEntitlements.push_back(weaponHash);
        return true;
    }
    bool MaintainRemoteMount(
        const PlayerMountStatePayload& state,
        const std::optional<PlayerMountStatePayload>& localState) noexcept override {
        remoteMountStates.push_back(state);
        remoteMountLocalStates.push_back(localState);
        return true;
    }
    void ClearRemoteMount() noexcept override {
        ++remoteMountClearCalls;
    }
    bool SpawnWorldEntityProxy(
        const WorldEntityStatePayload& state) noexcept override {
        worldEntitySpawns.push_back(state);
        return true;
    }
    bool UpdateWorldEntityProxy(
        const WorldEntityStatePayload& state) noexcept override {
        worldEntityUpdates.push_back(state);
        return true;
    }
    void DespawnWorldEntityProxy(
        const NetEntityId entityId) noexcept override {
        worldEntityDespawns.push_back(entityId);
    }
    void MaintainWorldMirrorGuest(
        const bool active,
        const bool authoritativePopulationReady,
        float) noexcept override {
        worldMirrorGuestActive = active;
        worldMirrorAuthorityReady =
            active && authoritativePopulationReady;
        ++worldMirrorMaintainCalls;
    }
    bool ApplyWorldEntityDamage(
        const LocalEntityHandle target,
        const float damage) noexcept override {
        appliedWorldDamage.emplace_back(target, damage);
        return applyWorldDamageSucceeds;
    }
    bool ApplyMissionWorldEntityDamage(
        const LocalEntityHandle target,
        const std::uint32_t weaponHash,
        const float damage) noexcept override {
        appliedMissionWorldDamage.emplace_back(
            target,
            weaponHash,
            damage);
        return applyMissionWorldDamageSucceeds;
    }
    void MaintainRealtimeSession(
        const bool active,
        const bool synchronizedPaused) noexcept override {
        realtimeSessionActive = active;
        synchronizedPauseActive =
            active && synchronizedPaused;
        ++realtimePolicyCalls;
    }
    GuestMissionIsolationStatus MaintainMissionAuthority(
        const bool active,
        const bool hostMissionActive,
        const bool hostPresentationActive) noexcept override {
        missionAuthorityActive = active;
        hostMissionAuthorityActive =
            active && hostMissionActive;
        hostMissionPresentationActive =
            active && hostPresentationActive;
        ++missionAuthorityCalls;
        missionAuthorityStates.push_back(active);
        missionAuthorityHostMissionStates.push_back(
            active && hostMissionActive);
        missionAuthorityPresentationStates.push_back(
            active && hostPresentationActive);
        if (!active) {
            missionGateAsserted = false;
            missionGateStates.push_back(false);
            return {};
        }
        auto status = missionIsolationStatus;
        status.missionGateAsserted =
            ShouldAssertGuestMissionGate(
                active,
                hostMissionActive,
                hostPresentationActive,
                status.quarantineActive,
                status.localStoryInteractionSuppressed);
        missionGateAsserted = status.missionGateAsserted;
        missionGateStates.push_back(status.missionGateAsserted);
        return status;
    }
    void MaintainMissionSpectator(
        const bool active) noexcept override {
        missionSpectatorActive = active;
        missionSpectatorStates.push_back(active);
    }
    void MaintainMissionResumeBarrier(
        const bool active) noexcept override {
        missionResumeBarrierActive = active;
        missionResumeBarrierStates.push_back(active);
    }
    MissionResumePreparation PrepareMissionCinematicResume(
        const Vec3& anchor,
        const float heading,
        const std::uint64_t nowMs) noexcept override {
        resumeAnchors.push_back(anchor);
        resumeHeadings.push_back(heading);
        resumePreparationTicks.push_back(nowMs);
        return missionResumePreparation;
    }
    bool IsCutsceneSkipPressed() noexcept override {
        const bool pressed = cutsceneSkipPressed;
        cutsceneSkipPressed = false;
        return pressed;
    }
    void MaintainCutsceneSkipInput(const bool active) noexcept override {
        cutsceneSkipInputActive = active;
        cutsceneSkipInputStates.push_back(active);
    }
    void MaintainReplicatedMissionCamera(
        const bool spectatorActive,
        const std::optional<MissionCameraStatePayload>& state) noexcept override {
        replicatedMissionCameraSpectatorActive = spectatorActive;
        replicatedMissionCameraState = state;
        replicatedMissionCameraCalls.emplace_back(
            spectatorActive,
            state);
    }
    bool MaintainReplicatedAnimScene(
        const bool spectatorActive,
        const std::optional<AnimSceneReplicaStatePayload>& state) noexcept override {
        replicatedAnimSceneSpectatorActive = spectatorActive;
        replicatedAnimSceneState = state;
        replicatedAnimSceneCalls.emplace_back(
            spectatorActive,
            state);
        return replicatedAnimSceneAvailable && spectatorActive &&
               state.has_value();
    }
    ReplicatedAnimScenePrepareResult
    PrepareReplicatedAnimSceneDefinition(
        const AnimSceneDefinitionPayload& definition,
        const NetEntityId localEntityId) noexcept override {
        ++replicatedAnimScenePrepareCalls;
        try {
            lastPreparedAnimSceneDefinition = definition;
            lastPreparedAnimSceneLocalEntityId = localEntityId;
        } catch (...) {
            return ReplicatedAnimScenePrepareResult{
                ReplicatedAnimScenePrepareStatus::NativeFailure};
        }
        return replicatedAnimScenePrepareResult;
    }
    bool MaintainHostAnimSceneStartBarrier(
        const bool active) noexcept override {
        hostAnimSceneStartBarrierActive = active;
        hostAnimSceneStartBarrierStates.push_back(active);
        return !active || hostAnimSceneStartBarrierResult;
    }
    bool CommitReplicatedAnimSceneDefinition(
        const AnimSceneControlPayload& commit) noexcept override {
        ++replicatedAnimSceneCommitCalls;
        try {
            lastReplicatedAnimSceneCommit = commit;
        } catch (...) {
            return false;
        }
        return replicatedAnimSceneCommitResult;
    }
    void AbortReplicatedAnimSceneDefinition() noexcept override {
        ++replicatedAnimSceneAbortCalls;
    }
    void MaintainMissionCompanionPresentation(
        const MissionCompanionPresentation& state) noexcept override {
        missionCompanionPresentation = state;
        missionCompanionPresentationStates.push_back(state);
    }
    void MaintainRemoteMissionParticipant(
        const bool hidden) noexcept override {
        remoteMissionParticipantHidden = hidden;
        remoteMissionParticipantStates.push_back(hidden);
    }
    void RequestCheckpointRetry() noexcept override { ++retryCount; }
    void Log(std::string_view text) noexcept override {
        try {
            logs.emplace_back(text);
        } catch (...) {
        }
    }
    void WaitForNextTick() noexcept override {}

    std::uint64_t tick{1'000U};
    std::uint64_t tickReadAdvance{};
    RuntimeMode mode{true, true, false};
    LocalPlayerSample sample{};
    bool sampleAvailable{true};
    std::optional<PlayerAppearanceStatePayload> sampledAppearance{};
    std::optional<MissionCameraSample> sampledMissionCamera{};
    std::optional<AnimSceneReplicaStatePayload> sampledAnimScene{};
    std::optional<LocalEntityHandle> sampledAnimSceneLocalHandle{};
    std::vector<CapturedAnimSceneDefinition>
        capturedAnimSceneDefinitions{};
    std::unordered_map<LocalEntityHandle, NetEntityId>
        knownReplicaNetworkIds{};
    RuntimeDivergenceDiagnostics runtimeDivergenceDiagnostics{};
    std::optional<PlayerAnimationStatePayload> sampledAnimation{};
    std::optional<WorldStatePayload> sampledWorld{
        WorldStatePayload{
            12U,
            0U,
            0U,
            0U,
            0U,
            0U,
            0.0F}};
    std::optional<CampaignMissionProbe> sampledCampaignMission{};
    bool applyCampaignMissionCompletionResult{};
    std::size_t campaignMissionCompletionApplyCount{};
    std::uint32_t appliedCampaignMissionId{};
    std::uint64_t appliedCampaignMissionEventId{};
    std::uint8_t appliedCampaignMissionRating{};
    std::optional<std::int32_t> localCashBalance{100};
    bool applyCampaignMissionCashAwardResult{true};
    std::size_t campaignMissionCashApplyCount{};
    std::uint64_t appliedCampaignMissionCashEventId{};
    std::int32_t appliedCampaignMissionCashAmount{};
    std::optional<float> distance{25.0F};
    std::size_t retryCount{};
    std::vector<std::string> logs{};
    std::vector<BridgeCommand> localCommands{};
    std::vector<CommandPayload> networkCommands{};
    std::vector<PlayerStatePayload> remoteTransforms{};
    std::vector<PlayerAnimationStatePayload> remoteAnimationStates{};
    std::vector<MotionReplicationConfigPayload>
        motionReplicationConfigs{};
    std::optional<bool> animSceneCaptureHostAuthority{};
    std::size_t animSceneCaptureAuthorityChanges{};
    std::vector<PlayerTraversalPayload> remoteTraversals{};
    std::vector<PlayerActionPayload> remotePlayerActions{};
    std::vector<RestraintStatePayload> appliedRestraintStates{};
    std::vector<NetEntityId> restraintLocalEntityIds{};
    std::vector<PlayerIdentityPayload> remoteIdentities{};
    std::vector<WorldStatePayload> appliedWorldStates{};
    std::vector<EquipmentStatePayload> remoteEquipment{};
    std::vector<std::uint32_t> weaponEntitlements{};
    std::vector<PlayerMountStatePayload> remoteMountStates{};
    std::vector<std::optional<PlayerMountStatePayload>>
        remoteMountLocalStates{};
    std::vector<HostWorldEntitySample> sampledWorldEntities{};
    std::optional<DamageIntentPayload> pendingWorldDamageIntent{};
    std::vector<CampaignCapabilityObservation>
        pendingCampaignCapabilityObservations{};
    std::vector<WorldEntityStatePayload> worldEntitySpawns{};
    std::vector<WorldEntityStatePayload> worldEntityUpdates{};
    std::vector<NetEntityId> worldEntityDespawns{};
    std::vector<std::pair<LocalEntityHandle, float>>
        appliedWorldDamage{};
    std::vector<
        std::tuple<LocalEntityHandle, std::uint32_t, float>>
        appliedMissionWorldDamage{};
    std::vector<BridgeHudState> hudStates{};
    std::vector<std::string> notifications{};
    std::vector<PauseVoteView> pauseVoteStates{};
    bool realtimeSessionActive{};
    bool worldMirrorAuthorityReady{};
    bool synchronizedPauseActive{};
    bool applyRemoteTransformSucceeds{true};
    bool applyWorldDamageSucceeds{true};
    bool applyMissionWorldDamageSucceeds{true};
    bool worldMirrorGuestActive{};
    std::size_t worldMirrorMaintainCalls{};
    std::size_t realtimePolicyCalls{};
    std::size_t remoteMountClearCalls{};
    std::size_t missionAuthorityCalls{};
    bool missionAuthorityActive{};
    bool hostMissionAuthorityActive{};
    bool hostMissionPresentationActive{};
    bool missionGateAsserted{};
    std::vector<bool> missionGateStates{};
    std::vector<bool> missionAuthorityStates{};
    std::vector<bool> missionAuthorityHostMissionStates{};
    std::vector<bool> missionAuthorityPresentationStates{};
    bool missionSpectatorActive{};
    std::vector<bool> missionSpectatorStates{};
    bool missionResumeBarrierActive{};
    std::vector<bool> missionResumeBarrierStates{};
    MissionResumePreparation missionResumePreparation{true, false};
    std::vector<Vec3> resumeAnchors{};
    std::vector<float> resumeHeadings{};
    std::vector<std::uint64_t> resumePreparationTicks{};
    bool cutsceneSkipPressed{};
    bool cutsceneSkipInputActive{};
    std::vector<bool> cutsceneSkipInputStates{};
    bool replicatedMissionCameraSpectatorActive{};
    std::optional<MissionCameraStatePayload>
        replicatedMissionCameraState{};
    std::vector<std::pair<
        bool,
        std::optional<MissionCameraStatePayload>>>
        replicatedMissionCameraCalls{};
    bool replicatedAnimSceneAvailable{};
    bool replicatedAnimSceneSpectatorActive{};
    std::optional<AnimSceneReplicaStatePayload>
        replicatedAnimSceneState{};
    std::vector<std::pair<
        bool,
        std::optional<AnimSceneReplicaStatePayload>>>
        replicatedAnimSceneCalls{};
    ReplicatedAnimScenePrepareResult replicatedAnimScenePrepareResult{};
    std::optional<AnimSceneDefinitionPayload>
        lastPreparedAnimSceneDefinition{};
    NetEntityId lastPreparedAnimSceneLocalEntityId{};
    std::size_t replicatedAnimScenePrepareCalls{};
    bool replicatedAnimSceneCommitResult{true};
    std::optional<AnimSceneControlPayload>
        lastReplicatedAnimSceneCommit{};
    std::size_t replicatedAnimSceneCommitCalls{};
    std::size_t replicatedAnimSceneAbortCalls{};
    bool hostAnimSceneStartBarrierResult{true};
    bool hostAnimSceneStartBarrierActive{};
    std::vector<bool> hostAnimSceneStartBarrierStates{};
    MissionCompanionPresentation missionCompanionPresentation{};
    std::vector<MissionCompanionPresentation>
        missionCompanionPresentationStates{};
    GuestMissionIsolationStatus missionIsolationStatus{};
    bool remoteMissionParticipantHidden{};
    std::vector<bool> remoteMissionParticipantStates{};
    std::string clipboard{};
    bool sessionMenuOpen{};
    SessionOverlayPhase sessionMenuPhase{
        SessionOverlayPhase::WaitingForSidecar};
    std::string sessionMenuStatus{};
    std::size_t sessionMenuActionCount{};
    std::optional<SessionOverlayAction> sessionMenuFirstAction{};
    MenuInputState menuInput{};
};

class TestTransport final : public IFrameTransport {
public:
    bool Connect(std::string& error) override {
        if (!connectSucceeds) {
            error = "synthetic pipe unavailable";
            connected = false;
            return false;
        }
        error.clear();
        connected = true;
        return true;
    }
    void Disconnect() noexcept override { connected = false; }
    bool IsConnected() const noexcept override { return connected; }
    bool Send(const Frame& frame, std::string& error) override {
        if (failNextEntityDespawn &&
            frame.header.type == MessageType::EntityDespawn) {
            failNextEntityDespawn = false;
            connected = false;
            error = "synthetic pipe send failure during entity despawn";
            return false;
        }
        error.clear();
        sent.push_back(frame);
        if (frame.header.type == MessageType::Hello &&
            acknowledgeHello) {
            Frame acknowledgement;
            acknowledgement.header.type = MessageType::HelloAck;
            acknowledgement.header.sequence = ++inboundSequence;
            acknowledgement.header.tick = frame.header.tick;
            acknowledgement.payload = acknowledgementPayload;
            inbound.push_back(std::move(acknowledgement));
            if (injectRemoteState) {
                Frame remoteState;
                remoteState.header.type = MessageType::PlayerState;
                // Same number as HelloAck is valid: pipe control and remote
                // LAN streams have independent sequence domains.
                remoteState.header.sequence = inboundSequence;
                remoteState.header.tick = frame.header.tick;
                remoteState.payload = EncodePlayerState(
                    PlayerStatePayload{
                        remoteEntityId,
                        remoteSlot,
                        PlayerLifecycle::Alive,
                        remotePosition,
                        {1.0F, 0.0F, 0.0F},
                        remoteHeading,
                        1.0F,
                        0U,
                        {},
                        0U,
                        0.0F,
                        0.0F,
                        0.0F,
                        0.0F,
                        remoteLocomotionEpoch});
                inbound.push_back(std::move(remoteState));
            }
        }
        return connected;
    }
    std::vector<Frame> Poll(std::string& error) override {
        if (disconnectOnNextPoll) {
            disconnectOnNextPoll = false;
            connected = false;
            error = "synthetic pipe receive failure";
            return {};
        }
        error.clear();
        auto result = std::move(inbound);
        inbound.clear();
        return result;
    }

    bool connected{};
    bool connectSucceeds{true};
    bool disconnectOnNextPoll{};
    bool failNextEntityDespawn{};
    bool acknowledgeHello{true};
    std::vector<std::uint8_t> acknowledgementPayload{
        static_cast<std::uint8_t>(PlayerSlot::Guest)};
    bool injectRemoteState{true};
    NetEntityId remoteEntityId{
        NetEntityId::Compose(0xA0B0C0D0U, 1U)};
    PlayerSlot remoteSlot{PlayerSlot::Host};
    Vec3 remotePosition{10.0F, 20.0F, 30.0F};
    float remoteHeading{180.0F};
    std::uint16_t remoteLocomotionEpoch{};
    std::uint32_t inboundSequence{};
    std::vector<Frame> sent{};
    std::vector<Frame> inbound{};
};

[[nodiscard]] bool HasLog(
    const TestFacade& facade,
    const std::string_view fragment) {
    return std::any_of(
        facade.logs.begin(),
        facade.logs.end(),
        [fragment](const std::string& line) {
            return line.find(fragment) != std::string::npos;
        });
}

[[nodiscard]] std::size_t CountLogs(
    const TestFacade& facade,
    const std::string_view fragment) {
    return static_cast<std::size_t>(std::count_if(
        facade.logs.begin(),
        facade.logs.end(),
        [fragment](const std::string& line) {
            return line.find(fragment) != std::string::npos;
        }));
}

[[nodiscard]] std::optional<CommandPayload> LastSentCommand(
    const TestTransport& transport,
    const CommandOpcode opcode) {
    for (auto frame = transport.sent.rbegin();
         frame != transport.sent.rend();
         ++frame) {
        if (frame->header.type != MessageType::Command) {
            continue;
        }
        const auto command = DecodeCommand(frame->payload);
        if (command.has_value() && command->opcode == opcode) {
            return command;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<PauseVotePayload> LastSentPauseVote(
    const TestTransport& transport) {
    for (auto frame = transport.sent.rbegin();
         frame != transport.sent.rend();
         ++frame) {
        if (frame->header.type != MessageType::PauseVote) {
            continue;
        }
        if (const auto pause =
                DecodePauseVote(frame->payload);
            pause.has_value()) {
            return pause;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<MissionStatePayload> LastSentMissionState(
    const TestTransport& transport) {
    for (auto frame = transport.sent.rbegin();
         frame != transport.sent.rend();
         ++frame) {
        if (frame->header.type != MessageType::MissionState) {
            continue;
        }
        if (const auto mission =
                DecodeMissionState(frame->payload);
            mission.has_value()) {
            return mission;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<MissionProgressionPayload>
LastSentMissionProgression(const TestTransport& transport) {
    for (auto frame = transport.sent.rbegin();
         frame != transport.sent.rend();
         ++frame) {
        if (frame->header.type != MessageType::MissionProgression) continue;
        if (const auto payload = DecodeMissionProgression(frame->payload);
            payload.has_value()) return payload;
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<MissionCameraStatePayload>
LastSentMissionCameraState(const TestTransport& transport) {
    for (auto frame = transport.sent.rbegin();
         frame != transport.sent.rend();
         ++frame) {
        if (frame->header.type !=
            MessageType::MissionCameraState) {
            continue;
        }
        if (const auto camera =
                DecodeMissionCameraState(frame->payload);
            camera.has_value()) {
            return camera;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<AnimSceneReplicaStatePayload>
LastSentAnimSceneReplicaState(const TestTransport& transport) {
    for (auto frame = transport.sent.rbegin();
         frame != transport.sent.rend();
         ++frame) {
        if (frame->header.type !=
            MessageType::AnimSceneReplicaState) {
            continue;
        }
        if (const auto state =
                DecodeAnimSceneReplicaState(frame->payload);
            state.has_value()) {
            return state;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<AnimSceneDefinitionPayload>
LastSentAnimSceneDefinition(const TestTransport& transport) {
    for (auto frame = transport.sent.rbegin();
         frame != transport.sent.rend();
         ++frame) {
        if (frame->header.type != MessageType::AnimSceneDefinition) {
            continue;
        }
        if (const auto definition =
                DecodeAnimSceneDefinition(frame->payload);
            definition.has_value()) {
            return definition;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<AnimSceneControlPayload>
LastSentAnimSceneControl(
    const TestTransport& transport,
    const AnimSceneControlKind kind) {
    for (auto frame = transport.sent.rbegin();
         frame != transport.sent.rend();
         ++frame) {
        if (frame->header.type != MessageType::AnimSceneControl) {
            continue;
        }
        if (const auto control =
                DecodeAnimSceneControl(frame->payload);
            control.has_value() && control->kind == kind) {
            return control;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<MissionCinematicStatePayload>
LastSentMissionCinematicState(const TestTransport& transport) {
    for (auto frame = transport.sent.rbegin();
         frame != transport.sent.rend();
         ++frame) {
        if (frame->header.type != MessageType::MissionCinematicState) {
            continue;
        }
        if (const auto state =
                DecodeMissionCinematicState(frame->payload);
            state.has_value()) {
            return state;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<MissionCinematicActionPayload>
LastSentMissionCinematicAction(
    const TestTransport& transport,
    const MissionCinematicActionKind kind) {
    for (auto frame = transport.sent.rbegin();
         frame != transport.sent.rend();
         ++frame) {
        if (frame->header.type != MessageType::MissionCinematicAction) {
            continue;
        }
        if (const auto action =
                DecodeMissionCinematicAction(frame->payload);
            action.has_value() && action->kind == kind) {
            return action;
        }
    }
    return std::nullopt;
}

void InvokeTeleportGuestMenu(
    BridgeRuntime& runtime,
    TestFacade& facade) {
    facade.menuInput.f9 = true;
    runtime.Tick();
    facade.menuInput = {};
    runtime.Tick();

    facade.menuInput.down = true;
    runtime.Tick();
    facade.menuInput = {};
    runtime.Tick();

    facade.menuInput.down = true;
    runtime.Tick();
    facade.menuInput = {};
    runtime.Tick();

    facade.menuInput.confirm = true;
    runtime.Tick();
    facade.menuInput = {};
}

void InvokeTeleportToPlayerMenu(
    BridgeRuntime& runtime,
    TestFacade& facade) {
    facade.menuInput.f9 = true;
    runtime.Tick();
    facade.menuInput = {};
    runtime.Tick();

    facade.menuInput.down = true;
    runtime.Tick();
    facade.menuInput = {};
    runtime.Tick();

    facade.menuInput.confirm = true;
    runtime.Tick();
    facade.menuInput = {};
}

void InvokeResyncEquipmentMenu(
    BridgeRuntime& runtime,
    TestFacade& facade) {
    facade.menuInput.f9 = true;
    runtime.Tick();
    facade.menuInput = {};
    runtime.Tick();

    for (int index = 0; index < 4; ++index) {
        facade.menuInput.down = true;
        runtime.Tick();
        facade.menuInput = {};
        runtime.Tick();
    }

    facade.menuInput.confirm = true;
    runtime.Tick();
    facade.menuInput = {};
}

void InvokeSaveProblemMarkerMenu(
    BridgeRuntime& runtime,
    TestFacade& facade) {
    facade.menuInput.f7 = true;
    runtime.Tick();
    runtime.Tick();
    facade.menuInput.f7 = false;
    runtime.Tick();
}

[[nodiscard]] std::size_t CountSentFrames(
    const TestTransport& transport,
    const MessageType type) {
    return static_cast<std::size_t>(
        std::count_if(
            transport.sent.begin(),
            transport.sent.end(),
            [type](const Frame& frame) {
                return frame.header.type == type;
            }));
}

void RuntimeUsesGameDesiredMoveBlendForScriptedSpeed() {
    TestFacade facade;
    facade.sample.velocity = {};
    facade.sample.desiredMoveBlend = 0.65F;
    facade.sample.desiredMoveBlendValid = true;
    TestTransport transport;
    BridgeRuntime runtime{facade, transport};
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;
    CHECK(runtime.Start(supported, error));
    runtime.Tick();

    std::optional<PlayerStatePayload> state;
    for (const auto& frame : transport.sent) {
        if (frame.header.type == MessageType::PlayerState) {
            state = DecodePlayerState(frame.payload);
        }
    }
    CHECK(state.has_value());
    CHECK(std::abs(state->desiredMoveBlend - 0.65F) < 0.001F);

    transport.sent.clear();
    facade.sample.desiredMoveBlendValid = false;
    facade.sample.velocity = {0.20F, 0.0F, 0.0F};
    facade.tick += 50U;
    runtime.Tick();
    for (const auto& frame : transport.sent) {
        if (frame.header.type == MessageType::PlayerState) {
            state = DecodePlayerState(frame.payload);
        }
    }
    CHECK(state.has_value());
    CHECK(state->desiredMoveBlend == 1.0F);
}

void RuntimeLoopback() {
    TestFacade facade;
    LocalMountSample localMount;
    localMount.localHandle = 101;
    localMount.modelHash = 0xA1B2C3D4U;
    localMount.position = {2.0F, 3.0F, 4.0F};
    localMount.velocity = {1.0F, 0.0F, 0.0F};
    localMount.heading = 90.0F;
    localMount.healthFraction = 0.8F;
    localMount.mounted = true;
    facade.sample.mount = localMount;
    facade.sample.mounted = true;
    TestTransport transport;
    BridgeRuntime runtime{facade, transport};
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;
    CHECK(runtime.Start(supported, error));
    CHECK(runtime.IsActive());
    CHECK(!runtime.LocalSlot().has_value());
    CHECK(transport.sent.front().header.type == MessageType::Hello);

    for (int index = 0; index < 11; ++index) {
        runtime.Tick();
        facade.tick += 10U;
    }
    std::size_t telemetryCount{};
    std::size_t mountTelemetryCount{};
    std::optional<PlayerMountStatePayload> lastLocalMount;
    for (const auto& frame : transport.sent) {
        telemetryCount +=
            frame.header.type == MessageType::PlayerState ? 1U : 0U;
        if (frame.header.type ==
            MessageType::PlayerMountState) {
            ++mountTelemetryCount;
            lastLocalMount =
                DecodePlayerMountState(frame.payload);
        }
    }
    CHECK(telemetryCount == 3U);
    CHECK(mountTelemetryCount == telemetryCount);
    CHECK(lastLocalMount.has_value());
    CHECK(lastLocalMount->slot == PlayerSlot::Guest);
    CHECK(lastLocalMount->mountEntityId.Counter() == 11U);
    CHECK(
        (lastLocalMount->flags &
         static_cast<std::uint8_t>(
             PlayerMountStateFlag::Mounted)) != 0U);
    CHECK(lastLocalMount->modelHash == 0xA1B2C3D4U);
    CHECK(
        (lastLocalMount->flags &
         static_cast<std::uint8_t>(
             PlayerMountStateFlag::BorrowedPeerMount)) == 0U);
    CHECK(runtime.LocalSlot() == PlayerSlot::Guest);
    CHECK(runtime.LocalEntityId().Counter() == 2U);
    std::optional<PlayerStatePayload> lastPlayerState;
    for (const auto& frame : transport.sent) {
        if (frame.header.type == MessageType::PlayerState) {
            lastPlayerState = DecodePlayerState(frame.payload);
        }
    }
    CHECK(lastPlayerState.has_value());
    CHECK(lastPlayerState->slot == PlayerSlot::Guest);
    CHECK(lastPlayerState->entityId.Counter() == 2U);
    CHECK(!facade.networkCommands.empty());
    CHECK(
        facade.networkCommands[0].opcode ==
        CommandOpcode::SpawnReplica);
    CHECK(facade.remoteTransforms.size() >= 1U);
    CHECK(
        facade.networkCommands[0].target ==
        NetEntityId::Compose(0xA0B0C0D0U, 1U));
    CHECK(
        facade.remoteTransforms[0].entityId ==
        NetEntityId::Compose(0xA0B0C0D0U, 1U));
    CHECK(!facade.hudStates.empty());
    CHECK(facade.hudStates.back().sidecarConnected);
    CHECK(facade.hudStates.back().remoteConnected);
    CHECK(
        facade.hudStates.back().localSlot ==
        PlayerSlot::Guest);

    facade.sample.mount->borrowedPeerMount = true;
    facade.sample.mount->sharedEntityId =
        NetEntityId::Compose(0xA0B0C0D0U, 10U);
    facade.sample.mount->sharedGeneration = 7U;
    facade.tick += 50U;
    runtime.Tick();
    const auto borrowedFrame = std::find_if(
        transport.sent.rbegin(),
        transport.sent.rend(),
        [](const Frame& frame) {
            return frame.header.type ==
                   MessageType::PlayerMountState;
        });
    CHECK(borrowedFrame != transport.sent.rend());
    const auto borrowedMount =
        DecodePlayerMountState(borrowedFrame->payload);
    CHECK(borrowedMount.has_value());
    CHECK(
        borrowedMount->mountEntityId ==
        facade.sample.mount->sharedEntityId);
    CHECK(borrowedMount->generation == 7U);
    CHECK(
        (borrowedMount->flags &
         static_cast<std::uint8_t>(
             PlayerMountStateFlag::BorrowedPeerMount)) != 0U);
    facade.sample.mount->borrowedPeerMount = false;
    facade.sample.mount->sharedEntityId = {};
    facade.sample.mount->sharedGeneration = 0U;

    facade.sample.mounted = false;
    facade.sample.jumpPressed = true;
    facade.sample.velocity = {4.0F, 0.0F, 0.0F};
    facade.tick += 10U;
    runtime.Tick();
    facade.sample.jumpPressed = false;
    CHECK(CountSentFrames(
              transport,
              MessageType::PlayerTraversal) == 1U);
    const auto sentTraversal = std::find_if(
        transport.sent.begin(),
        transport.sent.end(),
        [](const Frame& frame) {
            return frame.header.type ==
                   MessageType::PlayerTraversal;
        });
    CHECK(sentTraversal != transport.sent.end());
    const auto decodedSentTraversal =
        DecodePlayerTraversal(sentTraversal->payload);
    CHECK(decodedSentTraversal.has_value());
    CHECK(
        (decodedSentTraversal->flags &
         static_cast<std::uint32_t>(
             PlayerTraversalFlag::InputEdgeDetected)) != 0U);

    Frame identity;
    identity.header.type = MessageType::PlayerIdentity;
    identity.payload = EncodePlayerIdentity(
        PlayerIdentityPayload{
            transport.remoteEntityId,
            PlayerSlot::Host,
            "Host_Arthur"});
    transport.inbound.push_back(std::move(identity));
    runtime.Tick();
    CHECK(facade.remoteIdentities.size() == 1U);
    CHECK(
        facade.remoteIdentities.back().nickname ==
        "Host_Arthur");

    Frame traversal;
    traversal.header.type = MessageType::PlayerTraversal;
    traversal.payload = EncodePlayerTraversal(
        PlayerTraversalPayload{
            transport.remoteEntityId,
            PlayerSlot::Host,
            PlayerTraversalKind::Jump,
            3U,
            1U,
            2U,
            static_cast<std::uint32_t>(
                PlayerTraversalFlag::InputEdgeDetected),
            90.0F,
            {5.0F, 6.0F, 7.0F},
            {3.0F, 0.0F, 0.0F},
            {},
            {},
            0.0F,
            {}});
    transport.inbound.push_back(std::move(traversal));
    runtime.Tick();
    CHECK(facade.remoteTraversals.size() == 1U);
    CHECK(facade.remoteTraversals.back().actionId == 3U);

    facade.sample.aiming = true;
    facade.sample.aimTargetValid = true;
    facade.sample.aimTarget = {12.0F, 21.0F, 31.0F};
    facade.tick += 60U;
    runtime.Tick();
    std::optional<PlayerActionPayload> localAimBegin;
    for (auto iterator = transport.sent.rbegin();
         iterator != transport.sent.rend();
         ++iterator) {
        if (iterator->header.type != MessageType::PlayerAction) {
            continue;
        }
        const auto decoded = DecodePlayerAction(iterator->payload);
        if (decoded.has_value() &&
            decoded->kind == PlayerActionKind::Aim) {
            localAimBegin = decoded;
            break;
        }
    }
    CHECK(localAimBegin.has_value());
    CHECK(localAimBegin->phase == PlayerActionPhase::Begin);
    CHECK(
        (localAimBegin->flags &
         static_cast<std::uint32_t>(PlayerActionFlag::Intent)) != 0U);
    CHECK(
        (localAimBegin->flags & static_cast<std::uint32_t>(
             PlayerActionFlag::NormalizedPhaseValid)) != 0U);
    CHECK(localAimBegin->normalizedPhase == 0.0F);

    facade.sample.aiming = false;
    facade.sample.aimTargetValid = false;
    facade.sample.aimTarget = {};
    facade.tick += 60U;
    runtime.Tick();
    std::optional<PlayerActionPayload> localAimEnd;
    for (auto iterator = transport.sent.rbegin();
         iterator != transport.sent.rend();
         ++iterator) {
        if (iterator->header.type != MessageType::PlayerAction) {
            continue;
        }
        const auto decoded = DecodePlayerAction(iterator->payload);
        if (decoded.has_value() &&
            decoded->kind == PlayerActionKind::Aim &&
            decoded->phase == PlayerActionPhase::End) {
            localAimEnd = decoded;
            break;
        }
    }
    CHECK(localAimEnd.has_value());
    CHECK(localAimEnd->actionId == localAimBegin->actionId);
    CHECK(localAimEnd->revision > localAimBegin->revision);
    CHECK(localAimEnd->normalizedPhase > 0.0F);
    CHECK(localAimEnd->normalizedPhase <= 1.0F);

    facade.sample.meleeCombat = true;
    facade.sample.meleeAttackPressed = true;
    facade.sample.peerCombatTarget = true;
    facade.tick += 20U;
    runtime.Tick();
    std::optional<PlayerActionPayload> firstMeleeBegin;
    for (auto iterator = transport.sent.rbegin();
         iterator != transport.sent.rend();
         ++iterator) {
        if (iterator->header.type != MessageType::PlayerAction) {
            continue;
        }
        const auto decoded = DecodePlayerAction(iterator->payload);
        if (decoded.has_value() &&
            decoded->kind == PlayerActionKind::MeleeAttack &&
            decoded->phase == PlayerActionPhase::Begin) {
            firstMeleeBegin = decoded;
            break;
        }
    }
    CHECK(firstMeleeBegin.has_value());
    CHECK(firstMeleeBegin->durationMilliseconds == 750U);
    CHECK(firstMeleeBegin->targetEntityId == transport.remoteEntityId);

    // A second explicit click is a new epoch; it may not extend the first
    // autonomous task/channel indefinitely.
    facade.tick += 20U;
    runtime.Tick();
    std::optional<PlayerActionPayload> secondMeleeBegin;
    for (auto iterator = transport.sent.rbegin();
         iterator != transport.sent.rend();
         ++iterator) {
        if (iterator->header.type != MessageType::PlayerAction) {
            continue;
        }
        const auto decoded = DecodePlayerAction(iterator->payload);
        if (decoded.has_value() &&
            decoded->kind == PlayerActionKind::MeleeAttack &&
            decoded->phase == PlayerActionPhase::Begin &&
            decoded->actionId != firstMeleeBegin->actionId) {
            secondMeleeBegin = decoded;
            break;
        }
    }
    CHECK(secondMeleeBegin.has_value());

    facade.sample.meleeCombat = false;
    facade.sample.meleeAttackPressed = false;
    facade.sample.peerCombatTarget = false;
    facade.tick += 20U;
    runtime.Tick();
    std::optional<PlayerActionPayload> meleeEnd;
    for (auto iterator = transport.sent.rbegin();
         iterator != transport.sent.rend();
         ++iterator) {
        if (iterator->header.type != MessageType::PlayerAction) {
            continue;
        }
        const auto decoded = DecodePlayerAction(iterator->payload);
        if (decoded.has_value() &&
            decoded->kind == PlayerActionKind::MeleeAttack &&
            decoded->phase == PlayerActionPhase::End) {
            meleeEnd = decoded;
            break;
        }
    }
    CHECK(meleeEnd.has_value());
    CHECK(meleeEnd->actionId == secondMeleeBegin->actionId);

    facade.sample.peerCombatTarget = true;
    facade.sample.peerKnockdown = true;
    facade.tick += 20U;
    runtime.Tick();
    std::optional<PlayerActionPayload> knockdownBegin;
    for (auto iterator = transport.sent.rbegin();
         iterator != transport.sent.rend();
         ++iterator) {
        if (iterator->header.type != MessageType::PlayerAction) {
            continue;
        }
        const auto decoded = DecodePlayerAction(iterator->payload);
        if (decoded.has_value() &&
            decoded->kind == PlayerActionKind::Knockdown &&
            decoded->phase == PlayerActionPhase::Begin) {
            knockdownBegin = decoded;
            break;
        }
    }
    CHECK(knockdownBegin.has_value());
    CHECK(knockdownBegin->durationMilliseconds == 750U);
    CHECK(
        (knockdownBegin->flags &
         static_cast<std::uint32_t>(
             PlayerActionFlag::PhysicalTargetEffect)) != 0U);
    CHECK(
        (knockdownBegin->flags &
         static_cast<std::uint32_t>(
             PlayerActionFlag::VariantValid)) == 0U);
    CHECK(knockdownBegin->variantHash == 0U);
    facade.sample.peerCombatTarget = false;
    facade.sample.peerKnockdown = false;
    facade.tick += 20U;
    runtime.Tick();

    facade.sample.peerCombatTarget = true;
    facade.sample.peerKnockdown = true;
    facade.sample.peerMountPull = true;
    facade.tick += 20U;
    runtime.Tick();
    std::optional<PlayerActionPayload> mountPullBegin;
    for (auto iterator = transport.sent.rbegin();
         iterator != transport.sent.rend();
         ++iterator) {
        if (iterator->header.type != MessageType::PlayerAction) {
            continue;
        }
        const auto decoded = DecodePlayerAction(iterator->payload);
        if (decoded.has_value() &&
            decoded->kind == PlayerActionKind::Knockdown &&
            decoded->phase == PlayerActionPhase::Begin &&
            decoded->actionId != knockdownBegin->actionId) {
            mountPullBegin = decoded;
            break;
        }
    }
    CHECK(mountPullBegin.has_value());
    CHECK(
        (mountPullBegin->flags &
         static_cast<std::uint32_t>(
             PlayerActionFlag::VariantValid)) != 0U);
    CHECK(
        mountPullBegin->variantHash ==
        kPlayerActionVariantPeerMountPull);
    facade.sample.peerCombatTarget = false;
    facade.sample.peerKnockdown = false;
    facade.sample.peerMountPull = false;
    facade.tick += 20U;
    runtime.Tick();

    // A lasso throw names the peer before the local engine confirms a catch.
    // The receiver can therefore start its native wind-up/rope task on Begin;
    // PhysicalTargetEffect upgrades the same action only after restraint.
    facade.sample.weaponHash = 0xAABBCCDDU;
    facade.sample.peerCombatTarget = true;
    facade.sample.peerLassoIntent = true;
    facade.sample.peerLassoActive = false;
    facade.tick += 20U;
    runtime.Tick();
    std::optional<PlayerActionPayload> lassoBegin;
    for (auto iterator = transport.sent.rbegin();
         iterator != transport.sent.rend();
         ++iterator) {
        if (iterator->header.type != MessageType::PlayerAction) {
            continue;
        }
        const auto decoded = DecodePlayerAction(iterator->payload);
        if (decoded.has_value() &&
            decoded->kind == PlayerActionKind::Lasso &&
            decoded->phase == PlayerActionPhase::Begin) {
            lassoBegin = decoded;
            break;
        }
    }
    CHECK(lassoBegin.has_value());
    CHECK(lassoBegin->targetEntityId == transport.remoteEntityId);
    CHECK(
        (lassoBegin->flags & static_cast<std::uint32_t>(
             PlayerActionFlag::TargetEntityValid)) != 0U);
    CHECK(
        (lassoBegin->flags & static_cast<std::uint32_t>(
             PlayerActionFlag::PhysicalTargetEffect)) == 0U);

    facade.sample.peerLassoActive = true;
    facade.tick += 600U;
    runtime.Tick();
    std::optional<PlayerActionPayload> lassoCaught;
    for (auto iterator = transport.sent.rbegin();
         iterator != transport.sent.rend();
         ++iterator) {
        if (iterator->header.type != MessageType::PlayerAction) {
            continue;
        }
        const auto decoded = DecodePlayerAction(iterator->payload);
        if (decoded.has_value() &&
            decoded->kind == PlayerActionKind::Lasso &&
            decoded->actionId == lassoBegin->actionId &&
            decoded->phase == PlayerActionPhase::Sustain) {
            lassoCaught = decoded;
            break;
        }
    }
    CHECK(lassoCaught.has_value());
    CHECK(
        (lassoCaught->flags & static_cast<std::uint32_t>(
             PlayerActionFlag::PhysicalTargetEffect)) != 0U);
    facade.sample.peerLassoIntent = false;
    facade.sample.peerLassoActive = false;
    facade.sample.peerCombatTarget = false;
    facade.tick += 20U;
    runtime.Tick();

    PlayerActionPayload remoteAttack{
        transport.remoteEntityId,
        runtime.LocalEntityId(),
        1U,
        77U,
        1U,
        PlayerSlot::Host,
        PlayerSlot::Host,
        PlayerActionKind::MeleeAttack,
        PlayerActionPhase::Begin,
        static_cast<std::uint32_t>(PlayerActionFlag::Authoritative) |
            static_cast<std::uint32_t>(
                PlayerActionFlag::TargetEntityValid) |
            static_cast<std::uint32_t>(
                PlayerActionFlag::TargetPointValid) |
            static_cast<std::uint32_t>(
                PlayerActionFlag::ActorAnchorValid),
        10'000U,
        0U,
        0U,
        0U,
        0U,
        {10.0F, 20.0F, 30.0F},
        {2.0F, 3.0F, 4.0F},
        180.0F,
        0.0F};
    Frame remoteAttackBegin;
    remoteAttackBegin.header.type = MessageType::PlayerAction;
    remoteAttackBegin.payload = EncodePlayerAction(remoteAttack);
    transport.inbound.push_back(std::move(remoteAttackBegin));
    runtime.Tick();
    CHECK(facade.remotePlayerActions.size() == 1U);
    CHECK(facade.remotePlayerActions.back().actionId == 77U);

    Frame duplicateRemoteAttack;
    duplicateRemoteAttack.header.type = MessageType::PlayerAction;
    duplicateRemoteAttack.payload = EncodePlayerAction(remoteAttack);
    transport.inbound.push_back(std::move(duplicateRemoteAttack));
    runtime.Tick();
    CHECK(facade.remotePlayerActions.size() == 1U);

    remoteAttack.sequence = 2U;
    remoteAttack.revision = 2U;
    remoteAttack.phase = PlayerActionPhase::End;
    Frame remoteAttackEnd;
    remoteAttackEnd.header.type = MessageType::PlayerAction;
    remoteAttackEnd.payload = EncodePlayerAction(remoteAttack);
    transport.inbound.push_back(std::move(remoteAttackEnd));
    runtime.Tick();
    CHECK(facade.remotePlayerActions.size() == 2U);
    CHECK(
        facade.remotePlayerActions.back().phase ==
        PlayerActionPhase::End);

    Frame world;
    world.header.type = MessageType::WorldState;
    world.payload = EncodeWorldState(
        WorldStatePayload{
            18U,
            45U,
            30U,
            0U,
            0U,
            0U,
            0.0F});
    transport.inbound.push_back(std::move(world));
    Frame equipment;
    equipment.header.type = MessageType::EquipmentState;
    equipment.payload = EncodeEquipmentState(
        EquipmentStatePayload{
            transport.remoteEntityId,
            0xAABBCCDDU,
            24U,
            static_cast<std::uint32_t>(
                EquipmentStateFlag::Equipped)});
    transport.inbound.push_back(std::move(equipment));
    Frame remoteMount;
    remoteMount.header.type =
        MessageType::PlayerMountState;
    remoteMount.header.sequence =
        ++transport.inboundSequence;
    remoteMount.payload = EncodePlayerMountState(
        PlayerMountStatePayload{
            transport.remoteEntityId,
            NetEntityId::Compose(0xA0B0C0D0U, 10U),
            PlayerSlot::Host,
            static_cast<std::uint8_t>(
                PlayerMountStateFlag::Present) |
                static_cast<std::uint8_t>(
                    PlayerMountStateFlag::Mounted),
            0x10203040U,
            {10.0F, 20.0F, 30.0F},
            {1.0F, 0.0F, 0.0F},
            180.0F,
            1.0F,
            1U});
    transport.inbound.push_back(std::move(remoteMount));
    runtime.Tick();
    CHECK(facade.appliedWorldStates.size() == 1U);
    CHECK(facade.appliedWorldStates.back().hour == 18U);
    CHECK(facade.remoteEquipment.size() == 1U);
    CHECK(
        facade.remoteEquipment.back().weaponHash ==
        0xAABBCCDDU);
    CHECK(facade.weaponEntitlements.size() == 1U);
    CHECK(facade.weaponEntitlements.back() == 0xAABBCCDDU);
    CHECK(facade.remoteMountStates.size() == 1U);
    CHECK(facade.remoteMountLocalStates.size() == 1U);
    CHECK(facade.remoteMountLocalStates.back().has_value());
    CHECK(
        facade.remoteMountStates.back().modelHash ==
        0x10203040U);
    runtime.Tick();
    CHECK(facade.remoteMountStates.size() == 1U);
    facade.tick += 49U;
    runtime.Tick();
    CHECK(facade.remoteMountStates.size() == 1U);
    // Spectator release is intentionally debounced so that a brief UI or
    // control handoff cannot flicker the guest back into the local mission.
    facade.tick += 651U;
    runtime.Tick();
    CHECK(facade.remoteMountStates.size() == 2U);

    const auto clearCallsBeforeAbsent =
        facade.remoteMountClearCalls;
    Frame absentRemoteMount;
    absentRemoteMount.header.type =
        MessageType::PlayerMountState;
    absentRemoteMount.header.sequence =
        ++transport.inboundSequence;
    absentRemoteMount.payload = EncodePlayerMountState(
        PlayerMountStatePayload{
            transport.remoteEntityId,
            NetEntityId::Compose(0xA0B0C0D0U, 10U),
            PlayerSlot::Host,
            0U,
            0U,
            {},
            {},
            0.0F,
            0.0F,
            1U});
    transport.inbound.push_back(std::move(absentRemoteMount));
    runtime.Tick();
    CHECK(facade.remoteMountClearCalls == clearCallsBeforeAbsent);

    const auto refreshRemotePlayer = [&]() {
        Frame remotePlayer;
        remotePlayer.header.type = MessageType::PlayerState;
        remotePlayer.header.sequence =
            ++transport.inboundSequence;
        remotePlayer.header.tick = facade.tick;
        remotePlayer.payload = EncodePlayerState(
            PlayerStatePayload{
                transport.remoteEntityId,
                PlayerSlot::Host,
                PlayerLifecycle::Alive,
                transport.remotePosition,
                {},
                transport.remoteHeading,
                1.0F,
                0U});
        transport.inbound.push_back(std::move(remotePlayer));
    };

    // Personal horses can disappear from Story Mode's local pools for
    // several seconds around doors, towns and transitions. Keep the player
    // stream fresh while verifying the full eight-second absence debounce;
    // otherwise the independent five-second replica timeout would clear it.
    facade.tick += 4'000U;
    refreshRemotePlayer();
    runtime.Tick();
    CHECK(facade.remoteMountClearCalls == clearCallsBeforeAbsent);
    facade.tick += 3'900U;
    refreshRemotePlayer();
    runtime.Tick();
    CHECK(facade.remoteMountClearCalls == clearCallsBeforeAbsent);
    facade.tick += 101U;
    runtime.Tick();
    CHECK(
        facade.remoteMountClearCalls ==
        clearCallsBeforeAbsent + 1U);
    Frame repeatedAbsentRemoteMount;
    repeatedAbsentRemoteMount.header.type =
        MessageType::PlayerMountState;
    repeatedAbsentRemoteMount.header.sequence =
        ++transport.inboundSequence;
    repeatedAbsentRemoteMount.payload = EncodePlayerMountState(
        PlayerMountStatePayload{
            transport.remoteEntityId,
            NetEntityId::Compose(0xA0B0C0D0U, 10U),
            PlayerSlot::Host,
            0U,
            0U,
            {},
            {},
            0.0F,
            0.0F,
            1U});
    transport.inbound.push_back(
        std::move(repeatedAbsentRemoteMount));
    runtime.Tick();
    facade.tick += 800U;
    runtime.Tick();
    CHECK(
        facade.remoteMountClearCalls ==
        clearCallsBeforeAbsent + 1U);

    facade.tick += 1'001U;
    runtime.Tick();
    CHECK(!facade.hudStates.back().remoteConnected);
    CHECK(runtime.RemoteConnected());

    facade.mode.onlineSessionActive = true;
    runtime.Tick();
    CHECK(!runtime.IsActive());
    CHECK(!transport.connected);
    CHECK(
        facade.networkCommands.back().opcode ==
        CommandOpcode::DespawnReplica);
}

void RuntimeAnimGraphModeIsIndependent() {
    TestFacade facade;
    facade.sampledAnimation = PlayerAnimationStatePayload{
        NetEntityId::Compose(1U, 1U),
        PlayerSlot::Guest,
        kPlayerAnimationStateSchemaVersion,
        PlayerAnimationSampleSource::LocomotionNative,
        1U,
        1U,
        static_cast<std::uint32_t>(
            PlayerAnimationCapability::StateIdentifier),
        static_cast<std::uint32_t>(
            PlayerAnimationStateFlag::StateHashValid),
        0U,
        0x9072A713U};
    TestTransport transport;
    transport.remoteLocomotionEpoch = 1U;
    BridgeRuntime runtime{facade, transport};
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;
    CHECK(runtime.Start(supported, error));
    runtime.Tick();
    CHECK(runtime.LocalSlot() == PlayerSlot::Guest);
    CHECK(runtime.RemoteConnected());

    Frame configure;
    configure.header.type = MessageType::MotionReplicationConfig;
    configure.header.sequence = ++transport.inboundSequence;
    configure.payload = EncodeMotionReplicationConfig(
        MotionReplicationConfigPayload{
            kMotionReplicationConfigSchemaVersion,
            MotionReplicationWireMode::AnimGraphReplica,
            static_cast<std::uint16_t>(
                MotionReplicationConfigFlag::EnableAnimSceneStoryVmProbe),
            1U});
    transport.inbound.push_back(std::move(configure));
    runtime.Tick();
    CHECK(facade.motionReplicationConfigs.size() == 1U);
    CHECK(
        facade.motionReplicationConfigs.back().mode ==
        MotionReplicationWireMode::AnimGraphReplica);
    CHECK(
        (facade.motionReplicationConfigs.back().flags &
         static_cast<std::uint16_t>(
             MotionReplicationConfigFlag::EnableAnimSceneStoryVmProbe)) != 0U);

    Frame toggleProbeOff;
    toggleProbeOff.header.type = MessageType::MotionReplicationConfig;
    toggleProbeOff.header.sequence = ++transport.inboundSequence;
    toggleProbeOff.payload = EncodeMotionReplicationConfig(
        MotionReplicationConfigPayload{
            kMotionReplicationConfigSchemaVersion,
            MotionReplicationWireMode::AnimGraphReplica,
            0U,
            1U});
    transport.inbound.push_back(std::move(toggleProbeOff));
    runtime.Tick();
    CHECK(facade.motionReplicationConfigs.size() == 2U);
    CHECK(facade.motionReplicationConfigs.back().flags == 0U);

    PlayerAnimationStatePayload remoteAnimation{
        transport.remoteEntityId,
        PlayerSlot::Host,
        kPlayerAnimationStateSchemaVersion,
        PlayerAnimationSampleSource::LocomotionNative,
        1U,
        7U,
        static_cast<std::uint32_t>(
            PlayerAnimationCapability::StateIdentifier),
        static_cast<std::uint32_t>(
            PlayerAnimationStateFlag::StateHashValid),
        0U,
        0x50F1BB2AU};
    Frame animation;
    animation.header.type = MessageType::PlayerAnimationState;
    animation.header.sequence = ++transport.inboundSequence;
    animation.payload = EncodePlayerAnimationState(remoteAnimation);
    transport.inbound.push_back(std::move(animation));
    runtime.Tick();
    // The animation lane is timeline-aligned with the next rendered
    // PlayerState rather than being forced immediately from the receive path.
    runtime.Tick();
    CHECK(!facade.remoteAnimationStates.empty());
    CHECK(
        facade.remoteAnimationStates.back().stateHash ==
        remoteAnimation.stateHash);

    const auto acceptedStateHash = remoteAnimation.stateHash;
    const auto appliedAfterFirstSample =
        facade.remoteAnimationStates.size();
    Frame duplicatePayload;
    duplicatePayload.header.type = MessageType::PlayerAnimationState;
    duplicatePayload.header.sequence = ++transport.inboundSequence;
    remoteAnimation.stateHash = 0x11111111U;
    // A fresh outer frame must not make a duplicate payload sequence current.
    duplicatePayload.payload = EncodePlayerAnimationState(remoteAnimation);
    transport.inbound.push_back(std::move(duplicatePayload));
    runtime.Tick();
    CHECK(!facade.remoteAnimationStates.empty());
    CHECK(
        facade.remoteAnimationStates.back().stateHash ==
        acceptedStateHash);

    Frame stalePayload;
    stalePayload.header.type = MessageType::PlayerAnimationState;
    stalePayload.header.sequence = ++transport.inboundSequence;
    remoteAnimation.sampleSequence = 6U;
    remoteAnimation.stateHash = 0x22222222U;
    stalePayload.payload = EncodePlayerAnimationState(remoteAnimation);
    transport.inbound.push_back(std::move(stalePayload));
    runtime.Tick();
    CHECK(
        facade.remoteAnimationStates.back().stateHash ==
        acceptedStateHash);

    Frame newerPayload;
    newerPayload.header.type = MessageType::PlayerAnimationState;
    newerPayload.header.sequence = ++transport.inboundSequence;
    remoteAnimation.sampleSequence = 8U;
    remoteAnimation.stateHash = 0x33333333U;
    newerPayload.payload = EncodePlayerAnimationState(remoteAnimation);
    transport.inbound.push_back(std::move(newerPayload));
    runtime.Tick();
    runtime.Tick();
    CHECK(
        facade.remoteAnimationStates.back().stateHash ==
        remoteAnimation.stateHash);
    CHECK(
        facade.remoteAnimationStates.size() >
        appliedAfterFirstSample);

    const auto appliedBeforeExpiry =
        facade.remoteAnimationStates.size();
    facade.tick += kRemoteAnimationStateCacheTtlMs + 1U;
    runtime.Tick();
    CHECK(
        facade.remoteAnimationStates.size() ==
        appliedBeforeExpiry);

    facade.tick += 60U;
    runtime.Tick();
    CHECK(
        CountSentFrames(
            transport,
            MessageType::PlayerAnimationState) >= 1U);

    const auto appliedBeforeDisable =
        facade.remoteAnimationStates.size();
    Frame disable;
    disable.header.type = MessageType::MotionReplicationConfig;
    disable.header.sequence = ++transport.inboundSequence;
    disable.payload = EncodeMotionReplicationConfig(
        MotionReplicationConfigPayload{
            kMotionReplicationConfigSchemaVersion,
            MotionReplicationWireMode::TaskNavmesh,
            0U,
            2U});
    transport.inbound.push_back(std::move(disable));
    runtime.Tick();
    CHECK(
        facade.motionReplicationConfigs.back().mode ==
        MotionReplicationWireMode::TaskNavmesh);

    Frame rejectedAnimation;
    rejectedAnimation.header.type =
        MessageType::PlayerAnimationState;
    rejectedAnimation.header.sequence = ++transport.inboundSequence;
    remoteAnimation.sampleSequence = 8U;
    rejectedAnimation.payload =
        EncodePlayerAnimationState(remoteAnimation);
    transport.inbound.push_back(std::move(rejectedAnimation));
    runtime.Tick();
    CHECK(
        facade.remoteAnimationStates.size() ==
        appliedBeforeDisable);
}

void RuntimeGuestPipeReconnectPreservesOutboundContinuity() {
    TestFacade facade;
    facade.sampledAnimation = PlayerAnimationStatePayload{
        NetEntityId::Compose(1U, 1U),
        PlayerSlot::Guest,
        kPlayerAnimationStateSchemaVersion,
        PlayerAnimationSampleSource::LocomotionNative,
        1U,
        1U,
        static_cast<std::uint32_t>(
            PlayerAnimationCapability::StateIdentifier),
        static_cast<std::uint32_t>(
            PlayerAnimationStateFlag::StateHashValid),
        0U,
        0x9072A713U};
    facade.sampledAppearance = PlayerAppearanceStatePayload{
        {},
        PlayerSlot::Guest,
        1U,
        static_cast<std::uint16_t>(
            PlayerAppearanceStateFlag::CompleteComponentSet) |
            static_cast<std::uint16_t>(
                PlayerAppearanceStateFlag::StoryMetaPed),
        4U,
        0xAABBCCDDU,
        0x0102030405060708ULL,
        {0x11111111U, 0x22222222U}};
    TestTransport transport;
    transport.remoteLocomotionEpoch = 1U;
    BridgeRuntime runtime{facade, transport};
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;
    CHECK(runtime.Start(supported, error));
    runtime.Tick();
    CHECK(runtime.LocalSlot() == PlayerSlot::Guest);

    Frame configure;
    configure.header.type = MessageType::MotionReplicationConfig;
    configure.header.sequence = ++transport.inboundSequence;
    configure.payload = EncodeMotionReplicationConfig(
        MotionReplicationConfigPayload{
            kMotionReplicationConfigSchemaVersion,
            MotionReplicationWireMode::AnimGraphReplica,
            0U,
            1U});
    transport.inbound.push_back(std::move(configure));
    facade.tick += 50U;
    runtime.Tick();
    facade.tick += 50U;
    runtime.Tick();

    const auto lastAnimation = [&]() {
        std::optional<PlayerAnimationStatePayload> result;
        for (const auto& frame : transport.sent) {
            if (frame.header.type == MessageType::PlayerAnimationState) {
                result = DecodePlayerAnimationState(frame.payload);
            }
        }
        return result;
    };
    const auto lastAppearance = [&]() {
        std::optional<PlayerAppearanceStatePayload> result;
        for (const auto& frame : transport.sent) {
            if (frame.header.type == MessageType::PlayerAppearanceState) {
                result = DecodePlayerAppearanceState(frame.payload);
            }
        }
        return result;
    };
    const auto lastPlayerState = [&]() {
        std::optional<PlayerStatePayload> result;
        for (const auto& frame : transport.sent) {
            if (frame.header.type == MessageType::PlayerState) {
                result = DecodePlayerState(frame.payload);
            }
        }
        return result;
    };
    const auto animationBeforeReconnect = lastAnimation();
    const auto appearanceBeforeReconnect = lastAppearance();
    CHECK(animationBeforeReconnect.has_value());
    CHECK(animationBeforeReconnect->sampleSequence > 0U);
    CHECK(appearanceBeforeReconnect.has_value());

    facade.sample.jumpPressed = true;
    facade.sample.velocity = {4.0F, 0.0F, 0.0F};
    facade.tick += 20U;
    runtime.Tick();
    facade.sample.jumpPressed = false;
    std::optional<PlayerTraversalPayload> traversalBeforeReconnect;
    for (const auto& frame : transport.sent) {
        if (frame.header.type == MessageType::PlayerTraversal) {
            traversalBeforeReconnect = DecodePlayerTraversal(frame.payload);
        }
    }
    CHECK(traversalBeforeReconnect.has_value());

    facade.sample.weaponHash = 0x10203040U;
    facade.sample.weaponAmmo = 6U;
    facade.sample.firing = true;
    facade.tick += 50U;
    runtime.Tick();
    const auto fireBeforeReconnect = lastPlayerState();
    CHECK(fireBeforeReconnect.has_value());
    CHECK(fireBeforeReconnect->fireSequence > 0U);
    facade.sample.firing = false;
    facade.tick += 50U;
    runtime.Tick();

    facade.sample.peerCombatTarget = true;
    facade.sample.peerLassoIntent = true;
    facade.sample.peerLassoActive = false;
    facade.tick += 20U;
    runtime.Tick();
    std::optional<PlayerActionPayload> lassoBegin;
    for (auto iterator = transport.sent.rbegin();
         iterator != transport.sent.rend();
         ++iterator) {
        if (iterator->header.type != MessageType::PlayerAction) {
            continue;
        }
        const auto action = DecodePlayerAction(iterator->payload);
        if (action.has_value() &&
            action->kind == PlayerActionKind::Lasso &&
            action->phase == PlayerActionPhase::Begin) {
            lassoBegin = action;
            break;
        }
    }
    CHECK(lassoBegin.has_value());
    facade.sample.peerLassoActive = true;
    facade.tick += 600U;
    runtime.Tick();

    // Keep the LAN session alive while the game pipe is unavailable for a
    // complete tick. Releasing lasso during that outage must be remembered.
    transport.connectSucceeds = false;
    transport.connected = false;
    facade.sample.peerCombatTarget = false;
    facade.sample.peerLassoIntent = false;
    facade.sample.peerLassoActive = false;
    facade.sampledAppearance->fingerprint = 0x1112131415161718ULL;
    facade.sampledAppearance->componentHashes.push_back(0x33333333U);
    facade.tick += 20U;
    runtime.Tick();
    const auto sentDuringOutage = transport.sent.size();

    transport.connectSucceeds = true;
    facade.tick += 1'001U;
    runtime.Tick();
    CHECK(transport.sent.size() > sentDuringOutage);
    std::optional<PlayerActionPayload> lassoEnd;
    for (auto iterator = transport.sent.rbegin();
         iterator != transport.sent.rend();
         ++iterator) {
        if (iterator->header.type != MessageType::PlayerAction) {
            continue;
        }
        const auto action = DecodePlayerAction(iterator->payload);
        if (action.has_value() &&
            action->kind == PlayerActionKind::Lasso &&
            action->phase == PlayerActionPhase::End) {
            lassoEnd = action;
            break;
        }
    }
    CHECK(lassoEnd.has_value());
    CHECK(lassoEnd->actionId == lassoBegin->actionId);
    CHECK(lassoEnd->sequence > lassoBegin->sequence);

    const auto animationAfterReconnect = lastAnimation();
    const auto appearanceAfterReconnect = lastAppearance();
    CHECK(animationAfterReconnect.has_value());
    CHECK(
        animationAfterReconnect->sampleSequence >
        animationBeforeReconnect->sampleSequence);
    CHECK(appearanceAfterReconnect.has_value());
    CHECK(
        appearanceAfterReconnect->revision >
        appearanceBeforeReconnect->revision);

    // A higher config revision on the new game-pipe generation does not mean
    // the LAN animation payload sequence may restart when the mode is unchanged.
    Frame reconnectConfig;
    reconnectConfig.header.type = MessageType::MotionReplicationConfig;
    reconnectConfig.header.sequence = ++transport.inboundSequence;
    reconnectConfig.payload = EncodeMotionReplicationConfig(
        MotionReplicationConfigPayload{
            kMotionReplicationConfigSchemaVersion,
            MotionReplicationWireMode::AnimGraphReplica,
            0U,
            2U});
    transport.inbound.push_back(std::move(reconnectConfig));
    facade.tick += 50U;
    runtime.Tick();
    const auto animationAfterConfig = lastAnimation();
    CHECK(animationAfterConfig.has_value());
    CHECK(
        animationAfterConfig->sampleSequence >
        animationAfterReconnect->sampleSequence);

    facade.sample.jumpPressed = true;
    facade.tick += 20U;
    runtime.Tick();
    facade.sample.jumpPressed = false;
    std::optional<PlayerTraversalPayload> traversalAfterReconnect;
    for (auto iterator = transport.sent.rbegin();
         iterator != transport.sent.rend();
         ++iterator) {
        if (iterator->header.type == MessageType::PlayerTraversal) {
            traversalAfterReconnect =
                DecodePlayerTraversal(iterator->payload);
            break;
        }
    }
    CHECK(traversalAfterReconnect.has_value());
    CHECK(
        traversalAfterReconnect->actionId >
        traversalBeforeReconnect->actionId);
    CHECK(
        traversalAfterReconnect->locomotionEpoch >
        traversalBeforeReconnect->locomotionEpoch);

    facade.sample.firing = true;
    facade.sample.weaponAmmo = 5U;
    facade.tick += 50U;
    runtime.Tick();
    const auto fireAfterReconnect = lastPlayerState();
    CHECK(fireAfterReconnect.has_value());
    CHECK(
        fireAfterReconnect->fireSequence >
        fireBeforeReconnect->fireSequence);
}

void RuntimeHudFreshnessToleratesInTickClockAdvance() {
    TestFacade facade;
    facade.tickReadAdvance = 5U;
    TestTransport transport;
    BridgeRuntime runtime{facade, transport};
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;
    CHECK(runtime.Start(supported, error));

    runtime.Tick();
    CHECK(!facade.hudStates.empty());
    CHECK(facade.hudStates.back().remoteConnected);
    CHECK(facade.remoteTransforms.size() == 1U);
}

void RuntimeDefersRestraintUntilRemoteSpawn() {
    TestFacade facade;
    TestTransport transport;
    transport.injectRemoteState = false;
    BridgeRuntime runtime{facade, transport};
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;
    CHECK(runtime.Start(supported, error));
    runtime.Tick();
    CHECK(runtime.LocalSlot() == PlayerSlot::Guest);
    CHECK(!runtime.RemoteConnected());

    const RestraintStatePayload bound{
        transport.remoteEntityId,
        runtime.LocalEntityId(),
        81U,
        1U,
        PlayerRestraintState::Lassoed,
        static_cast<std::uint8_t>(
            RestraintStateFlag::Authoritative) |
            static_cast<std::uint8_t>(
                RestraintStateFlag::Snapshot)};
    Frame restraint;
    restraint.header.type = MessageType::RestraintState;
    restraint.header.sequence = ++transport.inboundSequence;
    restraint.header.tick = facade.tick;
    restraint.payload = EncodeRestraintState(bound);
    transport.inbound.push_back(std::move(restraint));
    runtime.Tick();

    CHECK(facade.appliedRestraintStates.empty());
    CHECK(HasLog(facade, "deferred unmatched subject"));

    const auto latestFree = RestraintStatePayload{
        bound.subjectEntityId,
        NetEntityId{},
        bound.sourceInteractionId,
        2U,
        PlayerRestraintState::Free,
        static_cast<std::uint8_t>(
            RestraintStateFlag::Authoritative) |
            static_cast<std::uint8_t>(
                RestraintStateFlag::Snapshot)};
    Frame latestRestraint;
    latestRestraint.header.type = MessageType::RestraintState;
    latestRestraint.header.sequence = ++transport.inboundSequence;
    latestRestraint.header.tick = facade.tick;
    latestRestraint.payload = EncodeRestraintState(latestFree);
    transport.inbound.push_back(std::move(latestRestraint));
    runtime.Tick();
    CHECK(facade.appliedRestraintStates.empty());

    Frame remoteState;
    remoteState.header.type = MessageType::PlayerState;
    remoteState.header.sequence = ++transport.inboundSequence;
    remoteState.header.tick = facade.tick;
    remoteState.payload = EncodePlayerState(
        PlayerStatePayload{
            transport.remoteEntityId,
            transport.remoteSlot,
            PlayerLifecycle::Alive,
            transport.remotePosition,
            {1.0F, 0.0F, 0.0F},
            transport.remoteHeading,
            1.0F,
            0U});
    transport.inbound.push_back(std::move(remoteState));
    runtime.Tick();

    CHECK(runtime.RemoteConnected());
    CHECK(facade.appliedRestraintStates.size() == 1U);
    CHECK(facade.appliedRestraintStates.front().subjectEntityId ==
          transport.remoteEntityId);
    CHECK(facade.appliedRestraintStates.front().state ==
          PlayerRestraintState::Free);
    CHECK(facade.appliedRestraintStates.front().revision == 2U);
    CHECK(facade.restraintLocalEntityIds.size() == 1U);
    CHECK(facade.restraintLocalEntityIds.front() ==
          runtime.LocalEntityId());
    CHECK(HasLog(facade, "applied deferred subject"));
}

void RuntimeClearsDeferredRestraintsAtSessionBoundaries() {
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    const auto queueUnmatchedBound = [](
        BridgeRuntime& runtime,
        TestFacade& facade,
        TestTransport& transport) {
        Frame restraint;
        restraint.header.type = MessageType::RestraintState;
        restraint.header.sequence = ++transport.inboundSequence;
        restraint.header.tick = facade.tick;
        restraint.payload = EncodeRestraintState(
            RestraintStatePayload{
                transport.remoteEntityId,
                runtime.LocalEntityId(),
                91U,
                1U,
                PlayerRestraintState::Lassoed,
                static_cast<std::uint8_t>(
                    RestraintStateFlag::Authoritative) |
                    static_cast<std::uint8_t>(
                        RestraintStateFlag::Snapshot)});
        transport.inbound.push_back(std::move(restraint));
        runtime.Tick();
    };
    const auto queueRemoteState = [](
        BridgeRuntime& runtime,
        TestFacade& facade,
        TestTransport& transport) {
        Frame remoteState;
        remoteState.header.type = MessageType::PlayerState;
        remoteState.header.sequence = ++transport.inboundSequence;
        remoteState.header.tick = facade.tick;
        remoteState.payload = EncodePlayerState(
            PlayerStatePayload{
                transport.remoteEntityId,
                transport.remoteSlot,
                PlayerLifecycle::Alive,
                transport.remotePosition,
                {1.0F, 0.0F, 0.0F},
                transport.remoteHeading,
                1.0F,
                0U});
        transport.inbound.push_back(std::move(remoteState));
        runtime.Tick();
    };

    {
        TestFacade facade;
        TestTransport transport;
        transport.injectRemoteState = false;
        BridgeRuntime runtime{facade, transport};
        std::string error;
        CHECK(runtime.Start(supported, error));
        runtime.Tick();
        queueUnmatchedBound(runtime, facade, transport);
        CHECK(facade.appliedRestraintStates.empty());

        const std::string message{"stopped"};
        Frame waiting;
        waiting.header.type = MessageType::SessionMenuStatus;
        waiting.header.sequence = ++transport.inboundSequence;
        waiting.header.tick = facade.tick;
        waiting.payload = {
            static_cast<std::uint8_t>(
                SessionMenuStatusKind::Waiting),
            static_cast<std::uint8_t>(message.size()),
            0U,
            0U,
            0U};
        waiting.payload.insert(
            waiting.payload.end(),
            message.begin(),
            message.end());
        transport.inbound.push_back(std::move(waiting));
        runtime.Tick();
        runtime.Tick();
        CHECK(runtime.LocalSlot() == PlayerSlot::Guest);

        queueRemoteState(runtime, facade, transport);
        CHECK(runtime.RemoteConnected());
        CHECK(facade.appliedRestraintStates.empty());
    }

    {
        TestFacade facade;
        TestTransport transport;
        transport.injectRemoteState = false;
        BridgeRuntime runtime{facade, transport};
        std::string error;
        CHECK(runtime.Start(supported, error));
        runtime.Tick();
        queueUnmatchedBound(runtime, facade, transport);
        CHECK(facade.appliedRestraintStates.empty());

        Frame goodbye;
        goodbye.header.type = MessageType::Goodbye;
        goodbye.header.sequence = ++transport.inboundSequence;
        goodbye.header.tick = facade.tick;
        transport.inbound.push_back(std::move(goodbye));
        runtime.Tick();
        CHECK(!transport.connected);

        facade.tick += 1'100U;
        runtime.Tick();
        CHECK(transport.connected);
        CHECK(runtime.LocalSlot() == PlayerSlot::Guest);

        queueRemoteState(runtime, facade, transport);
        CHECK(runtime.RemoteConnected());
        CHECK(facade.appliedRestraintStates.empty());
    }
}

void RuntimeRespawnsUnavailableReplica() {
    TestFacade facade;
    facade.applyRemoteTransformSucceeds = false;
    TestTransport transport;
    BridgeRuntime runtime{facade, transport};
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;
    CHECK(runtime.Start(supported, error));

    runtime.Tick();
    runtime.Tick();
    runtime.Tick();
    CHECK(!runtime.RemoteConnected());
    CHECK(
        HasLog(
            facade,
            "remote replica became unavailable"));

    facade.applyRemoteTransformSucceeds = true;
    Frame remoteState;
    remoteState.header.type = MessageType::PlayerState;
    remoteState.header.sequence = ++transport.inboundSequence;
    remoteState.header.tick = facade.tick;
    remoteState.payload = EncodePlayerState(
        PlayerStatePayload{
            transport.remoteEntityId,
            transport.remoteSlot,
            PlayerLifecycle::Alive,
            transport.remotePosition,
            {1.0F, 0.0F, 0.0F},
            transport.remoteHeading,
            1.0F,
            0U});
    transport.inbound.push_back(std::move(remoteState));
    runtime.Tick();
    CHECK(runtime.RemoteConnected());

    const auto spawnCount = std::count_if(
        facade.networkCommands.begin(),
        facade.networkCommands.end(),
        [](const CommandPayload& command) {
            return command.opcode ==
                   CommandOpcode::SpawnReplica;
        });
    CHECK(spawnCount == 2);
}

void RuntimeDespawnsStaleReplica() {
    TestFacade facade;
    TestTransport transport;
    BridgeRuntime runtime{facade, transport};
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;
    CHECK(runtime.Start(supported, error));
    runtime.Tick();
    CHECK(runtime.RemoteConnected());

    facade.tick += 5'001U;
    runtime.Tick();
    CHECK(!runtime.RemoteConnected());
    CHECK(
        HasLog(
            facade,
            "remote stream timed out"));
    CHECK(
        facade.networkCommands.back().opcode ==
        CommandOpcode::DespawnReplica);
}

void RuntimeAppliesHostWorldOutsideCutscenes() {
    TestFacade facade;
    TestTransport transport;
    BridgeRuntime runtime{facade, transport};
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;
    CHECK(runtime.Start(supported, error));
    runtime.Tick();

    const WorldStatePayload worldState{
        18U,
        20U,
        30U,
        0U,
        0U,
        0U,
        0.0F};
    Frame blockedWorld;
    blockedWorld.header.type = MessageType::WorldState;
    blockedWorld.payload = EncodeWorldState(worldState);
    facade.sample.cutsceneActive = true;
    transport.inbound.push_back(blockedWorld);
    runtime.Tick();
    CHECK(facade.appliedWorldStates.empty());
    CHECK(!facade.worldMirrorAuthorityReady);

    facade.sample.cutsceneActive = false;
    facade.sample.missionActive = true;
    Frame freeRoamWorld;
    freeRoamWorld.header.type = MessageType::WorldState;
    freeRoamWorld.payload = EncodeWorldState(worldState);
    transport.inbound.push_back(std::move(freeRoamWorld));
    runtime.Tick();
    CHECK(facade.appliedWorldStates.size() == 1U);
    facade.tick += 1U;
    runtime.Tick();
    CHECK(facade.worldMirrorAuthorityReady);
}

void RuntimeHostEmitsMissionStateTransitions() {
    TestFacade facade;
    facade.sample.position = {12.0F, 13.0F, 14.0F};
    facade.sample.heading = 725.0F;
    TestTransport transport;
    transport.acknowledgementPayload = {
        static_cast<std::uint8_t>(PlayerSlot::Host)};
    transport.remoteSlot = PlayerSlot::Guest;
    transport.remoteEntityId =
        NetEntityId::Compose(0xA0B0C0D0U, 2U);
    BridgeRuntime runtime{facade, transport};
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;
    CHECK(runtime.Start(supported, error));
    runtime.Tick();
    CHECK(runtime.LocalSlot() == PlayerSlot::Host);

    const auto idle = LastSentMissionState(transport);
    CHECK(idle.has_value());
    CHECK(idle->hostEntityId == runtime.LocalEntityId());
    CHECK(idle->missionEpoch != 0U);
    CHECK(idle->revision != 0U);
    CHECK(idle->checkpointGeneration != 0U);
    CHECK(idle->phase == MissionPhase::Idle);
    CHECK(
        (idle->flags &
         static_cast<std::uint8_t>(
             MissionStateFlag::MissionActive)) == 0U);
    CHECK(
        (idle->flags &
         static_cast<std::uint8_t>(
             MissionStateFlag::AnchorValid)) != 0U);
    CHECK(idle->hostAnchor.x == 12.0F);
    CHECK(idle->hostHeading == 5.0F);

    transport.sent.clear();
    facade.sample.missionActive = true;
    facade.sample.position = {21.0F, 22.0F, 23.0F};
    facade.sample.heading = 90.0F;
    facade.tick += 50U;
    runtime.Tick();
    const auto active = LastSentMissionState(transport);
    CHECK(active.has_value());
    CHECK(active->phase == MissionPhase::Active);
    CHECK(active->missionEpoch != idle->missionEpoch);
    CHECK(active->revision == 1U);
    CHECK(
        (active->flags &
         static_cast<std::uint8_t>(
             MissionStateFlag::MissionActive)) != 0U);
    CHECK(active->hostAnchor.x == 21.0F);
    CHECK(active->hostHeading == 90.0F);

    transport.sent.clear();
    facade.sample.position = {41.0F, 42.0F, 43.0F};
    facade.sample.heading = 125.0F;
    facade.tick += 1'001U;
    runtime.Tick();
    const auto liveAnchor = LastSentMissionState(transport);
    CHECK(liveAnchor.has_value());
    CHECK(liveAnchor->phase == MissionPhase::Active);
    CHECK(liveAnchor->missionEpoch == active->missionEpoch);
    CHECK(liveAnchor->revision > active->revision);
    CHECK(liveAnchor->hostAnchor.x == 41.0F);
    CHECK(liveAnchor->hostHeading == 125.0F);

    transport.sent.clear();
    transport.connectSucceeds = false;
    transport.disconnectOnNextPoll = true;
    facade.sample.position = {51.0F, 52.0F, 53.0F};
    facade.tick += 50U;
    runtime.Tick();
    CHECK(!transport.connected);
    facade.tick += 100U;
    runtime.Tick();
    transport.connectSucceeds = true;
    facade.tick += 1'001U;
    runtime.Tick();
    const auto reconnectedMission = LastSentMissionState(transport);
    CHECK(reconnectedMission.has_value());
    CHECK(reconnectedMission->phase == MissionPhase::Active);
    CHECK(reconnectedMission->missionEpoch == liveAnchor->missionEpoch);
    CHECK(reconnectedMission->revision > liveAnchor->revision);
    CHECK(reconnectedMission->hostAnchor.x == 51.0F);

    transport.sent.clear();
    facade.sample.cutsceneActive = true;
    facade.tick += 50U;
    runtime.Tick();
    const auto cutscene = LastSentMissionState(transport);
    CHECK(cutscene.has_value());
    CHECK(cutscene->phase == MissionPhase::Cutscene);
    CHECK(cutscene->missionEpoch == active->missionEpoch);
    CHECK(cutscene->revision > liveAnchor->revision);
    CHECK(
        (cutscene->flags &
         static_cast<std::uint8_t>(
             MissionStateFlag::MissionActive)) != 0U);
    CHECK(HasLog(facade, "[MISSION_TX][MISSION_FSM]"));
}

void RuntimeIgnoresNonMissionStoryLoadCamera() {
    TestFacade facade;
    // The regular Story Mode load briefly reports a cutscene camera before
    // a mission is active. It must not open a cinematic resume barrier.
    facade.sample.cutsceneActive = true;
    TestTransport transport;
    transport.acknowledgementPayload = {
        static_cast<std::uint8_t>(PlayerSlot::Host)};
    BridgeRuntime runtime{facade, transport};
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;
    CHECK(runtime.Start(supported, error));
    runtime.Tick();

    const auto mission = LastSentMissionState(transport);
    CHECK(mission.has_value());
    CHECK(mission->phase == MissionPhase::Idle);
    CHECK(!LastSentMissionCinematicState(transport).has_value());
    CHECK(!HasLog(facade, "[MISSION_RESUME_BARRIER]"));
}

void RuntimeHunt1MissionProgressionHandshake() {
    constexpr std::uint32_t kHunt1 = kHunt1MissionId;
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;

    TestFacade hostFacade;
    hostFacade.sample.missionActive = true;
    hostFacade.sampledCampaignMission = CampaignMissionProbe{kHunt1, true, false, false, 0U};
    TestTransport hostTransport;
    hostTransport.acknowledgementPayload = {static_cast<std::uint8_t>(PlayerSlot::Host)};
    BridgeRuntime host{hostFacade, hostTransport};
    CHECK(host.Start(supported, error));
    host.Tick();
    const auto offer = LastSentMissionProgression(hostTransport);
    CHECK(offer.has_value());
    CHECK(offer->phase == MissionProgressionPhase::Offer);
    CHECK(offer->missionId == kHunt1);

    TestFacade guestFacade;
    guestFacade.sampledCampaignMission = CampaignMissionProbe{kHunt1, false, true, false, 0U};
    TestTransport guestTransport;
    guestTransport.acknowledgementPayload = {static_cast<std::uint8_t>(PlayerSlot::Guest)};
    BridgeRuntime guest{guestFacade, guestTransport};
    CHECK(guest.Start(supported, error));
    guest.Tick();
    Frame offered;
    offered.header.type = MessageType::MissionProgression;
    offered.header.sequence = ++guestTransport.inboundSequence;
    offered.header.tick = guestFacade.tick;
    offered.payload = EncodeMissionProgression(*offer);
    guestTransport.inbound.push_back(std::move(offered));
    guest.Tick();
    const auto eligibility = LastSentMissionProgression(guestTransport);
    CHECK(eligibility.has_value());
    CHECK(eligibility->phase == MissionProgressionPhase::Eligibility);
    CHECK((eligibility->flags & static_cast<std::uint8_t>(
        MissionProgressionFlag::GuestCanStart)) != 0U);

    Frame eligible;
    eligible.header.type = MessageType::MissionProgression;
    eligible.header.sequence = ++hostTransport.inboundSequence;
    eligible.header.tick = hostFacade.tick;
    eligible.payload = EncodeMissionProgression(*eligibility);
    hostTransport.inbound.push_back(std::move(eligible));
    host.Tick();
    CHECK(HasLog(hostFacade, "host accepted guest mission eligibility"));

    hostTransport.sent.clear();
    hostFacade.sample.missionActive = false;
    hostFacade.sampledCampaignMission = CampaignMissionProbe{kHunt1, false, false, true, 4U};
    hostFacade.localCashBalance = 240;
    hostFacade.tick += 1'000U;
    host.Tick();
    const auto completion = LastSentMissionProgression(hostTransport);
    CHECK(completion.has_value());
    CHECK(completion->phase == MissionProgressionPhase::Completion);
    CHECK(completion->eventId == offer->eventId);
    CHECK((completion->flags & static_cast<std::uint8_t>(
        MissionProgressionFlag::VerifiedCompletionMapping)) != 0U);
    CHECK(completion->completionRating == 4U);
    CHECK(completion->completionCashAward == 140);

    // A future allow-listed save mapping is still constrained by the guest's
    // own preflight evidence and is exactly-once even if the completion is
    // retransmitted after a reconnect.
    guestFacade.applyCampaignMissionCompletionResult = true;
    Frame mappedCompletion;
    mappedCompletion.header.type = MessageType::MissionProgression;
    mappedCompletion.header.sequence = ++guestTransport.inboundSequence;
    mappedCompletion.header.tick = guestFacade.tick;
    mappedCompletion.payload = EncodeMissionProgression(MissionProgressionPayload{
        offer->missionId, offer->missionEpoch, offer->eventId,
        MissionProgressionPhase::Completion,
        static_cast<std::uint8_t>(MissionProgressionFlag::VerifiedCompletionMapping),
        4U, 140});
    guestTransport.inbound.push_back(mappedCompletion);
    guest.Tick();
    CHECK(guestFacade.campaignMissionCompletionApplyCount == 1U);
    CHECK(guestFacade.appliedCampaignMissionId == kHunt1);
    CHECK(guestFacade.appliedCampaignMissionEventId == offer->eventId);
    CHECK(guestFacade.appliedCampaignMissionRating == 4U);
    CHECK(guestFacade.campaignMissionCashApplyCount == 1U);
    CHECK(guestFacade.appliedCampaignMissionCashEventId == offer->eventId);
    CHECK(guestFacade.appliedCampaignMissionCashAmount == 140);

    mappedCompletion.header.sequence = ++guestTransport.inboundSequence;
    guestTransport.inbound.push_back(std::move(mappedCompletion));
    guest.Tick();
    CHECK(guestFacade.campaignMissionCompletionApplyCount == 1U);
    CHECK(guestFacade.campaignMissionCashApplyCount == 1U);

    // A cash write failure does not consume the event. Completion/reward
    // application is safe to repeat; the next delivery retries the missing
    // cash award rather than falsely recording a complete result.
    const MissionProgressionPayload retryOffer{
        kHunt1, offer->missionEpoch + 2U, offer->eventId + 2U,
        MissionProgressionPhase::Offer, 0U};
    guestFacade.sampledCampaignMission = CampaignMissionProbe{kHunt1, false, true, false, 0U};
    Frame retryOfferFrame;
    retryOfferFrame.header.type = MessageType::MissionProgression;
    retryOfferFrame.header.sequence = ++guestTransport.inboundSequence;
    retryOfferFrame.header.tick = guestFacade.tick;
    retryOfferFrame.payload = EncodeMissionProgression(retryOffer);
    guestTransport.inbound.push_back(std::move(retryOfferFrame));
    guest.Tick();
    guestFacade.applyCampaignMissionCashAwardResult = false;
    Frame retryCompletion;
    retryCompletion.header.type = MessageType::MissionProgression;
    retryCompletion.header.sequence = ++guestTransport.inboundSequence;
    retryCompletion.header.tick = guestFacade.tick;
    retryCompletion.payload = EncodeMissionProgression(MissionProgressionPayload{
        retryOffer.missionId, retryOffer.missionEpoch, retryOffer.eventId,
        MissionProgressionPhase::Completion,
        static_cast<std::uint8_t>(MissionProgressionFlag::VerifiedCompletionMapping),
        4U, 140});
    guestTransport.inbound.push_back(retryCompletion);
    guest.Tick();
    CHECK(guestFacade.campaignMissionCompletionApplyCount == 2U);
    CHECK(guestFacade.campaignMissionCashApplyCount == 2U);
    guestFacade.applyCampaignMissionCashAwardResult = true;
    retryCompletion.header.sequence = ++guestTransport.inboundSequence;
    guestTransport.inbound.push_back(std::move(retryCompletion));
    guest.Tick();
    CHECK(guestFacade.campaignMissionCompletionApplyCount == 3U);
    CHECK(guestFacade.campaignMissionCashApplyCount == 3U);

    guestFacade.sampledCampaignMission = CampaignMissionProbe{kHunt1, false, false, false, 0U};
    const MissionProgressionPayload ineligibleOffer{
        kHunt1, offer->missionEpoch + 1U, offer->eventId + 1U,
        MissionProgressionPhase::Offer, 0U};
    Frame rejectedOffer;
    rejectedOffer.header.type = MessageType::MissionProgression;
    rejectedOffer.header.sequence = ++guestTransport.inboundSequence;
    rejectedOffer.header.tick = guestFacade.tick;
    rejectedOffer.payload = EncodeMissionProgression(ineligibleOffer);
    guestTransport.inbound.push_back(std::move(rejectedOffer));
    guest.Tick();

    Frame rejectedCompletion;
    rejectedCompletion.header.type = MessageType::MissionProgression;
    rejectedCompletion.header.sequence = ++guestTransport.inboundSequence;
    rejectedCompletion.header.tick = guestFacade.tick;
    rejectedCompletion.payload = EncodeMissionProgression(MissionProgressionPayload{
        ineligibleOffer.missionId, ineligibleOffer.missionEpoch,
        ineligibleOffer.eventId, MissionProgressionPhase::Completion,
        static_cast<std::uint8_t>(MissionProgressionFlag::VerifiedCompletionMapping),
        4U, 140});
    guestTransport.inbound.push_back(std::move(rejectedCompletion));
    guest.Tick();
    CHECK(guestFacade.campaignMissionCompletionApplyCount == 3U);
    CHECK(guestFacade.campaignMissionCashApplyCount == 3U);
}

void RuntimeHostDebouncesScriptedControlPresentation() {
    TestFacade facade;
    facade.sample.position = {12.0F, 13.0F, 14.0F};
    facade.sample.missionActive = true;
    facade.sample.controlLocked = true;
    facade.sample.vehicleEntryTransition = true;
    TestTransport transport;
    transport.acknowledgementPayload = {
        static_cast<std::uint8_t>(PlayerSlot::Host)};
    transport.remoteSlot = PlayerSlot::Guest;
    transport.remoteEntityId =
        NetEntityId::Compose(0xA0B0C0D0U, 2U);
    BridgeRuntime runtime{facade, transport};
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;
    CHECK(runtime.Start(supported, error));

    runtime.Tick();
    const auto initial = LastSentMissionState(transport);
    CHECK(initial.has_value());
    CHECK(initial->phase == MissionPhase::Active);

    facade.tick += 149U;
    runtime.Tick();
    const auto shortLock = LastSentMissionState(transport);
    CHECK(shortLock.has_value());
    CHECK(shortLock->phase == MissionPhase::Active);

    facade.tick += 1U;
    runtime.Tick();
    const auto scriptedTransition = LastSentMissionState(transport);
    CHECK(scriptedTransition.has_value());
    CHECK(scriptedTransition->phase == MissionPhase::Cutscene);
    CHECK(
        (scriptedTransition->flags &
         static_cast<std::uint8_t>(
             MissionStateFlag::ScriptedControlLock)) != 0U);
    CHECK(
        (scriptedTransition->flags &
         static_cast<std::uint8_t>(
             MissionStateFlag::ScriptedVehicleTransition)) != 0U);
}

void RuntimeHostSpectatesMinigamesImmediately() {
    TestFacade facade;
    facade.sample.missionActive = true;
    facade.sample.minigameActive = true;
    TestTransport transport;
    transport.acknowledgementPayload = {
        static_cast<std::uint8_t>(PlayerSlot::Host)};
    transport.remoteSlot = PlayerSlot::Guest;
    transport.remoteEntityId =
        NetEntityId::Compose(0xA0B0C0D0U, 2U);
    BridgeRuntime runtime{facade, transport};
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;
    CHECK(runtime.Start(supported, error));
    runtime.Tick();
    const auto state = LastSentMissionState(transport);
    CHECK(state.has_value());
    CHECK(state->phase == MissionPhase::Cutscene);
    CHECK(
        (state->flags &
         static_cast<std::uint8_t>(
             MissionStateFlag::MinigameActivity)) != 0U);
}

void RuntimeVerticalMissionRewardCutsceneReconnect() {
    TestFacade facade;
    TestTransport transport;
    transport.acknowledgementPayload = {
        static_cast<std::uint8_t>(PlayerSlot::Host)};
    transport.remoteSlot = PlayerSlot::Guest;
    transport.remoteEntityId =
        NetEntityId::Compose(0xA0B0C0D0U, 2U);
    BridgeRuntime runtime{facade, transport};
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;
    CHECK(runtime.Start(supported, error));
    runtime.Tick();
    facade.sample.missionActive = true;
    facade.tick += 50U;
    runtime.Tick();
    facade.pendingCampaignCapabilityObservations.push_back(
        {CampaignCapabilityKind::WeaponShopEligibility, 1'674'213'418U});
    facade.tick += 50U;
    runtime.Tick();
    CHECK(std::any_of(transport.sent.begin(), transport.sent.end(),
        [](const Frame& frame) {
            return frame.header.type == MessageType::CampaignCapability;
        }));

    facade.sample.cutsceneActive = true;
    facade.tick += 50U;
    runtime.Tick();
    CHECK(LastSentMissionState(transport)->phase == MissionPhase::Cutscene);

    transport.connectSucceeds = false;
    transport.disconnectOnNextPoll = true;
    facade.tick += 50U;
    runtime.Tick();
    CHECK(!transport.connected);
    facade.tick += 100U;
    runtime.Tick();
    transport.connectSucceeds = true;
    const auto beforeReconnect = transport.sent.size();
    facade.tick += 1'001U;
    runtime.Tick();
    CHECK(transport.connected);
    const auto resent = std::find_if(
        transport.sent.begin() + static_cast<std::ptrdiff_t>(beforeReconnect),
        transport.sent.end(), [](const Frame& frame) {
            return frame.header.type == MessageType::MissionState &&
                DecodeMissionState(frame.payload)->phase == MissionPhase::Cutscene;
        });
    CHECK(resent != transport.sent.end());
}

void RuntimeHostForwardsObservedCampaignCapability() {
    TestFacade facade;
    TestTransport transport;
    transport.acknowledgementPayload = {
        static_cast<std::uint8_t>(PlayerSlot::Host)};
    transport.remoteSlot = PlayerSlot::Guest;
    transport.remoteEntityId =
        NetEntityId::Compose(0xA0B0C0D0U, 2U);
    BridgeRuntime runtime{facade, transport};
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;
    CHECK(runtime.Start(supported, error));
    runtime.Tick();
    CHECK(runtime.LocalSlot() == PlayerSlot::Host);

    transport.sent.clear();
    facade.pendingCampaignCapabilityObservations.push_back(
        CampaignCapabilityObservation{
            CampaignCapabilityKind::WeaponShopEligibility,
            1'674'213'418U});
    facade.tick += 1U;
    runtime.Tick();

    std::optional<CampaignCapabilityPayload> forwarded;
    for (const auto& frame : transport.sent) {
        if (frame.header.type == MessageType::CampaignCapability) {
            forwarded = DecodeCampaignCapability(frame.payload);
        }
    }
    CHECK(forwarded.has_value());
    CHECK(
        forwarded->kind ==
        CampaignCapabilityKind::WeaponShopEligibility);
    CHECK(forwarded->recordHash == 1'674'213'418U);
    CHECK(forwarded->hostEventId != 0U);
    CHECK(forwarded->grantedAtUnixMilliseconds > 0);
}

void RuntimeHostStreamsAndStopsMissionCamera() {
    TestFacade facade;
    facade.sample.position = {12.0F, 13.0F, 14.0F};
    facade.sampledMissionCamera = MissionCameraSample{
        {101.0F, 202.0F, 303.0F},
        {-12.0F, 0.0F, 179.0F},
        52.0F,
        static_cast<std::uint32_t>(
            MissionCameraStateFlag::SourceRenderingScriptCamera)};
    TestTransport transport;
    transport.acknowledgementPayload = {
        static_cast<std::uint8_t>(PlayerSlot::Host)};
    transport.remoteSlot = PlayerSlot::Guest;
    transport.remoteEntityId =
        NetEntityId::Compose(0xA0B0C0D0U, 2U);
    BridgeRuntime runtime{facade, transport};
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;
    CHECK(runtime.Start(supported, error));
    runtime.Tick();
    CHECK(runtime.LocalSlot() == PlayerSlot::Host);

    transport.sent.clear();
    facade.sample.missionActive = true;
    facade.sample.cutsceneActive = true;
    facade.tick += 50U;
    runtime.Tick();
    const auto loading = LastSentMissionCinematicState(transport);
    const auto cutscene = LastSentMissionState(transport);
    CHECK(loading.has_value());
    CHECK(loading->phase == MissionCinematicPhase::Loading);
    CHECK(LastSentMissionCameraState(transport).has_value());
    Frame presentationReady;
    presentationReady.header.type =
        MessageType::MissionCinematicAction;
    presentationReady.header.sequence = ++transport.inboundSequence;
    presentationReady.header.tick = facade.tick;
    presentationReady.payload = EncodeMissionCinematicAction(
        MissionCinematicActionPayload{
            runtime.LocalEntityId(),
            loading->missionEpoch,
            loading->cinematicGeneration,
            1U,
            MissionCinematicActionKind::PresentationReady,
            static_cast<std::uint8_t>(PlayerSlot::Guest),
            0U});
    transport.inbound.push_back(std::move(presentationReady));
    facade.tick += 1U;
    runtime.Tick();
    const auto first = LastSentMissionCameraState(transport);
    CHECK(first.has_value());
    CHECK(cutscene.has_value());
    CHECK(first->hostEntityId == runtime.LocalEntityId());
    CHECK(first->missionEpoch == cutscene->missionEpoch);
    CHECK(
        (first->flags & static_cast<std::uint32_t>(
             MissionCameraStateFlag::Active)) != 0U);
    CHECK(first->position.x == 101.0F);
    CHECK(first->rotation.z == 179.0F);
    CHECK(first->fieldOfView == 52.0F);
    CHECK(HasLog(facade, "[MISSION_CAMERA][TX]"));

    transport.sent.clear();
    facade.sampledMissionCamera = MissionCameraSample{
        {102.0F, 203.0F, 304.0F},
        {-10.0F, 1.0F, -179.0F},
        48.0F,
        static_cast<std::uint32_t>(
            MissionCameraStateFlag::SourceRenderingScriptCamera)};
    facade.tick += 50U;
    runtime.Tick();
    const auto second = LastSentMissionCameraState(transport);
    CHECK(second.has_value());
    CHECK(second->revision > first->revision);
    CHECK(second->position.x == 102.0F);

    transport.sent.clear();
    facade.sample.cutsceneActive = false;
    facade.tick += 50U;
    runtime.Tick();
    const auto stopped = LastSentMissionCameraState(transport);
    CHECK(stopped.has_value());
    CHECK(stopped->missionEpoch == first->missionEpoch);
    CHECK(stopped->revision > second->revision);
    CHECK(stopped->flags == 0U);
    CHECK(stopped->fieldOfView == 0.0F);
}

void RuntimeAnimSceneTransportPrefersNativeAndFallsBackToCamera() {
    constexpr auto kAnimSceneFlags =
        static_cast<std::uint32_t>(
            AnimSceneReplicaStateFlag::Active) |
        static_cast<std::uint32_t>(
            AnimSceneReplicaStateFlag::Running) |
        static_cast<std::uint32_t>(
            AnimSceneReplicaStateFlag::Loaded) |
        static_cast<std::uint32_t>(
            AnimSceneReplicaStateFlag::CameraActive) |
        static_cast<std::uint32_t>(
            AnimSceneReplicaStateFlag::OriginValid);
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;

    TestFacade hostFacade;
    hostFacade.sampledMissionCamera = MissionCameraSample{
        {101.0F, 102.0F, 103.0F},
        {-5.0F, 0.0F, 135.0F},
        55.0F,
        static_cast<std::uint32_t>(
            MissionCameraStateFlag::SourceRenderingScriptCamera)};
    hostFacade.sampledAnimScene = AnimSceneReplicaStatePayload{
        {},
        0U,
        0U,
        0U,
        0U,
        0xA17C5EEDU,
        kAnimSceneFlags,
        0.25F,
        42.0F,
        1.0F,
        {40.0F, 50.0F, 60.0F},
        {0.0F, 0.0F, 90.0F},
        1U};
    TestTransport hostTransport;
    hostTransport.acknowledgementPayload = {
        static_cast<std::uint8_t>(PlayerSlot::Host)};
    hostTransport.remoteSlot = PlayerSlot::Guest;
    BridgeRuntime hostRuntime{hostFacade, hostTransport};
    CHECK(hostRuntime.Start(supported, error));
    hostRuntime.Tick();
    CHECK(hostFacade.animSceneCaptureHostAuthority == true);
    hostTransport.sent.clear();
    hostFacade.sample.missionActive = true;
    hostFacade.sample.cutsceneActive = true;
    hostFacade.tick += 50U;
    hostRuntime.Tick();

    const auto hostCinematic =
        LastSentMissionCinematicState(hostTransport);
    const auto transmittedScene =
        LastSentAnimSceneReplicaState(hostTransport);
    CHECK(hostCinematic.has_value());
    CHECK(transmittedScene.has_value());
    CHECK(
        transmittedScene->hostEntityId ==
        hostRuntime.LocalEntityId());
    CHECK(
        transmittedScene->missionEpoch ==
        hostCinematic->missionEpoch);
    CHECK(
        transmittedScene->cinematicGeneration ==
        hostCinematic->cinematicGeneration);
    CHECK(transmittedScene->revision != 0U);
    CHECK(transmittedScene->dictionaryHash == 0xA17C5EEDU);
    CHECK(HasLog(hostFacade, "[ANIMSCENE_REPLICA][TX]"));

    TestFacade guestFacade;
    guestFacade.replicatedAnimSceneAvailable = true;
    TestTransport guestTransport;
    BridgeRuntime guestRuntime{guestFacade, guestTransport};
    CHECK(guestRuntime.Start(supported, error));
    guestRuntime.Tick();
    CHECK(guestFacade.animSceneCaptureHostAuthority == false);

    constexpr std::uint32_t kMissionEpoch = 7U;
    constexpr std::uint32_t kCinematicGeneration = 4U;
    constexpr auto kMissionAndAnchor =
        static_cast<std::uint8_t>(
            MissionStateFlag::MissionActive) |
        static_cast<std::uint8_t>(
            MissionStateFlag::AnchorValid);
    Frame mission;
    mission.header.type = MessageType::MissionState;
    mission.header.sequence = ++guestTransport.inboundSequence;
    mission.header.tick = guestFacade.tick;
    mission.payload = EncodeMissionState(
        MissionStatePayload{
            guestTransport.remoteEntityId,
            kMissionEpoch,
            1U,
            1U,
            MissionPhase::Cutscene,
            kMissionAndAnchor,
            {40.0F, 50.0F, 60.0F},
            90.0F});
    guestTransport.inbound.push_back(std::move(mission));

    const auto pushCinematic = [&](const MissionCinematicPhase phase,
                                   const std::uint32_t revision) {
        const bool anchorRequired =
            phase == MissionCinematicPhase::PrepareResume ||
            phase == MissionCinematicPhase::Completed;
        Frame frame;
        frame.header.type = MessageType::MissionCinematicState;
        frame.header.sequence = ++guestTransport.inboundSequence;
        frame.header.tick = guestFacade.tick;
        frame.payload = EncodeMissionCinematicState(
            MissionCinematicStatePayload{
                guestTransport.remoteEntityId,
                kMissionEpoch,
                kCinematicGeneration,
                revision,
                1U,
                phase,
                phase == MissionCinematicPhase::Loading ||
                        phase == MissionCinematicPhase::Playing
                    ? static_cast<std::uint16_t>(
                          MissionCinematicStateFlag::CameraExpected)
                    : (anchorRequired
                           ? static_cast<std::uint16_t>(
                                 MissionCinematicStateFlag::AnchorValid)
                           : 0U),
                anchorRequired
                    ? Vec3{40.0F, 50.0F, 60.0F}
                    : Vec3{},
                anchorRequired ? 90.0F : 0.0F});
        guestTransport.inbound.push_back(std::move(frame));
    };
    const auto pushCamera = [&](const std::uint32_t revision) {
        Frame frame;
        frame.header.type = MessageType::MissionCameraState;
        frame.header.sequence = ++guestTransport.inboundSequence;
        frame.header.tick = guestFacade.tick;
        frame.payload = EncodeMissionCameraState(
            MissionCameraStatePayload{
                guestTransport.remoteEntityId,
                kMissionEpoch,
                kCinematicGeneration,
                revision,
                static_cast<std::uint32_t>(
                    MissionCameraStateFlag::Active) |
                    static_cast<std::uint32_t>(
                        MissionCameraStateFlag::SourceRenderingScriptCamera),
                {101.0F, 102.0F, 103.0F},
                {-5.0F, 0.0F, 135.0F},
                55.0F});
        guestTransport.inbound.push_back(std::move(frame));
    };
    const auto pushAnimScene = [&](const std::uint32_t revision) {
        Frame frame;
        frame.header.type = MessageType::AnimSceneReplicaState;
        frame.header.sequence = ++guestTransport.inboundSequence;
        frame.header.tick = guestFacade.tick;
        frame.payload = EncodeAnimSceneReplicaState(
            AnimSceneReplicaStatePayload{
                guestTransport.remoteEntityId,
                kMissionEpoch,
                kCinematicGeneration,
                0U,
                revision,
                0xA17C5EEDU,
                kAnimSceneFlags,
                0.25F,
                42.0F,
                1.0F,
                {40.0F, 50.0F, 60.0F},
                {0.0F, 0.0F, 90.0F},
                1U});
        guestTransport.inbound.push_back(std::move(frame));
    };

    pushCinematic(MissionCinematicPhase::Playing, 1U);
    pushCamera(1U);
    pushAnimScene(1U);
    guestRuntime.Tick();
    CHECK(guestFacade.replicatedAnimSceneSpectatorActive);
    CHECK(guestFacade.replicatedAnimSceneState.has_value());
    CHECK(
        guestFacade.replicatedAnimSceneState->dictionaryHash ==
        0xA17C5EEDU);
    CHECK(!guestFacade.replicatedMissionCameraSpectatorActive);
    CHECK(!guestFacade.replicatedMissionCameraState.has_value());
    CHECK(HasLog(guestFacade, "fallback suspended"));

    guestFacade.replicatedAnimSceneAvailable = false;
    guestFacade.tick += 50U;
    guestRuntime.Tick();
    CHECK(guestFacade.replicatedMissionCameraSpectatorActive);
    CHECK(guestFacade.replicatedMissionCameraState.has_value());
    CHECK(HasLog(guestFacade, "using camera-keyframe fallback"));

    guestFacade.replicatedAnimSceneAvailable = true;
    guestFacade.tick += 751U;
    guestRuntime.Tick();
    CHECK(!guestFacade.replicatedAnimSceneState.has_value());
    CHECK(guestFacade.replicatedMissionCameraSpectatorActive);
    CHECK(guestFacade.replicatedMissionCameraState.has_value());

    pushAnimScene(2U);
    guestFacade.tick += 1U;
    guestRuntime.Tick();
    CHECK(guestFacade.replicatedAnimSceneState.has_value());
    CHECK(!guestFacade.replicatedMissionCameraSpectatorActive);

    pushCinematic(MissionCinematicPhase::Completed, 2U);
    guestFacade.tick += 1U;
    guestRuntime.Tick();
    CHECK(!guestFacade.replicatedAnimSceneSpectatorActive);
    CHECK(!guestFacade.replicatedAnimSceneState.has_value());
    CHECK(!guestFacade.replicatedMissionCameraSpectatorActive);
    CHECK(!guestFacade.replicatedMissionCameraState.has_value());
}

void RuntimeAnimSceneHybridTwoPhaseCommit() {
    constexpr std::uint32_t kDictionaryHash = 0xA17C5EEDU;
    constexpr std::uint32_t kHostPlayerModel = 0xAABB0001U;
    constexpr std::uint32_t kGuestPlayerModel = 0xAABB0002U;
    constexpr std::uint32_t kWorldActorModel = 0xAABB1000U;
    constexpr LocalEntityHandle kHostPlayerHandle = 101;
    constexpr LocalEntityHandle kGuestReplicaHandle = 202;
    constexpr LocalEntityHandle kWorldActorHandle = 303;
    constexpr auto kRequired = static_cast<std::uint16_t>(
        AnimSceneRoleFlag::Required);
    constexpr auto kPlayer = static_cast<std::uint16_t>(
        AnimSceneRoleFlag::Player);
    constexpr auto kAnimSceneFlags =
        static_cast<std::uint32_t>(
            AnimSceneReplicaStateFlag::Active) |
        static_cast<std::uint32_t>(
            AnimSceneReplicaStateFlag::Running) |
        static_cast<std::uint32_t>(
            AnimSceneReplicaStateFlag::Loaded) |
        static_cast<std::uint32_t>(
            AnimSceneReplicaStateFlag::CameraActive) |
        static_cast<std::uint32_t>(
            AnimSceneReplicaStateFlag::OriginValid);
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;

    TestFacade hostFacade;
    hostFacade.sample.localHandle = kHostPlayerHandle;
    hostFacade.sample.position = {10.0F, 20.0F, 30.0F};
    AnimSceneReplicaStatePayload sampledScene;
    sampledScene.dictionaryHash = kDictionaryHash;
    sampledScene.flags = kAnimSceneFlags;
    sampledScene.phase = 0.25F;
    sampledScene.durationSeconds = 42.0F;
    sampledScene.rate = 1.0F;
    sampledScene.originPosition = {10.0F, 20.0F, 30.0F};
    sampledScene.originRotation = {0.0F, 0.0F, 90.0F};
    sampledScene.activeCameraCount = 1U;
    hostFacade.sampledAnimScene = sampledScene;
    hostFacade.sampledAnimSceneLocalHandle = 77;

    HostWorldEntitySample worldActor;
    worldActor.localHandle = kWorldActorHandle;
    worldActor.modelHash = kWorldActorModel;
    worldActor.flags =
        static_cast<std::uint8_t>(WorldEntityStateFlag::Human) |
        static_cast<std::uint8_t>(WorldEntityStateFlag::ScriptOwned);
    worldActor.position = {12.0F, 20.0F, 30.0F};
    worldActor.heading = 90.0F;
    worldActor.taskKind = WorldTaskKind::Cinematic;
    worldActor.selectionPriority =
        HostWorldEntityPriority::ScriptOwned;
    worldActor.selectionDistanceMeters = 2.0F;
    HostWorldEntitySample sceneObject;
    sceneObject.localHandle = 404;
    sceneObject.modelHash = 0xAABB2000U;
    sceneObject.kind = WorldEntityKind::Object;
    sceneObject.flags = static_cast<std::uint8_t>(
        WorldEntityStateFlag::ScriptOwned);
    sceneObject.position = {13.0F, 20.0F, 30.0F};
    sceneObject.heading = 90.0F;
    sceneObject.taskKind = WorldTaskKind::Cinematic;
    sceneObject.selectionPriority =
        HostWorldEntityPriority::ScriptOwned;
    sceneObject.selectionDistanceMeters = 3.0F;

    TestTransport hostTransport;
    hostTransport.acknowledgementPayload = {
        static_cast<std::uint8_t>(PlayerSlot::Host)};
    hostTransport.remoteSlot = PlayerSlot::Guest;
    BridgeRuntime hostRuntime{hostFacade, hostTransport};
    CHECK(hostRuntime.Start(supported, error));
    hostRuntime.Tick();
    const auto guestEntityId = NetEntityId::Compose(
        hostRuntime.LocalEntityId().Epoch(),
        2U);
    hostFacade.knownReplicaNetworkIds.emplace(
        kGuestReplicaHandle,
        guestEntityId);
    hostFacade.sampledWorldEntities = {worldActor, sceneObject};

    CapturedAnimSceneDefinition capture;
    capture.captureSequence = 1U;
    capture.localSceneHandle = 77;
    capture.dictionaryHash = kDictionaryHash;
    capture.durationSeconds = 42.0F;
    capture.sceneFlags = 0x1020U;
    capture.createOptionFlags = 0x01U;
    capture.resourceName = "script@story@test_hybrid";
    capture.playbackList = "pl_main";
    // Intentionally non-canonical capture order. BridgeRuntime must sort by
    // role name before fingerprinting and transmission.
    capture.roles = {
        CapturedAnimSceneRoleBinding{
            "Dutch",
            kWorldActorHandle,
            kWorldActorModel,
            AnimSceneRoleKind::Ped,
            kRequired,
            0x30U},
        CapturedAnimSceneRoleBinding{
            "Companion",
            kGuestReplicaHandle,
            kGuestPlayerModel,
            AnimSceneRoleKind::Ped,
            static_cast<std::uint16_t>(kRequired | kPlayer),
            0x20U},
        CapturedAnimSceneRoleBinding{
            "Arthur",
            kHostPlayerHandle,
            kHostPlayerModel,
            AnimSceneRoleKind::Ped,
            static_cast<std::uint16_t>(kRequired | kPlayer),
            0x10U},
        // Real ODR1_INT captured 22 bindings, eight of which were scene-local
        // props. Protocol 20 gives those captured objects a bounded world
        // identity so the exact resource never starts with a partial cast.
        CapturedAnimSceneRoleBinding{
            "Table",
            404,
            0xAABB2000U,
            AnimSceneRoleKind::Object,
            kRequired,
            0x40U},
        // ODR1_INT releases several cigarette/bottle/weapon handles before
        // the bridge drains the completed capture. Those scene-local props
        // must remain optional instead of discarding the mapped actor cast.
        CapturedAnimSceneRoleBinding{
            "Cigarette",
            405,
            0U,
            AnimSceneRoleKind::Object,
            kRequired,
            0x50U}};
    capture.complete = true;
    hostFacade.capturedAnimSceneDefinitions.push_back(capture);

    hostTransport.sent.clear();
    hostFacade.sample.missionActive = true;
    hostFacade.sample.cutsceneActive = true;
    // Let the previously scheduled world-graph sample become due. The
    // hybrid lane runs after that sample and may then resolve the captured
    // world actor to the just-created stable NetEntityId.
    hostFacade.tick += 500U;
    hostRuntime.Tick();

    const auto hostMission = LastSentMissionState(hostTransport);
    const auto hostCinematic =
        LastSentMissionCinematicState(hostTransport);
    const auto definition =
        LastSentAnimSceneDefinition(hostTransport);
    CHECK(hostMission.has_value());
    CHECK(hostCinematic.has_value());
    CHECK(definition.has_value());
    CHECK(hostFacade.hostAnimSceneStartBarrierActive);
    CHECK(HasLog(hostFacade, "PRELOAD_BARRIER] armed"));
    CHECK(definition->hostEntityId == hostRuntime.LocalEntityId());
    CHECK(definition->missionEpoch == hostMission->missionEpoch);
    CHECK(
        definition->cinematicGeneration ==
        hostCinematic->cinematicGeneration);
    CHECK(definition->dictionaryHash == kDictionaryHash);
    CHECK(definition->roles.size() == 5U);
    CHECK(definition->roles[0].roleName == "Arthur");
    CHECK(definition->roles[1].roleName == "Cigarette");
    CHECK(definition->roles[2].roleName == "Companion");
    CHECK(definition->roles[3].roleName == "Dutch");
    CHECK(definition->roles[4].roleName == "Table");
    CHECK(
        definition->roles[0].entityId ==
        hostRuntime.LocalEntityId());
    CHECK(!definition->roles[1].entityId.IsValid());
    CHECK(definition->roles[1].modelHash == 0U);
    CHECK(definition->roles[1].bindingFlags == 0U);
    CHECK((definition->roles[1].flags & kRequired) == 0U);
    CHECK(definition->roles[2].entityId == guestEntityId);
    CHECK(
        definition->roles[3].entityId ==
        NetEntityId::Compose(
            hostRuntime.LocalEntityId().Epoch(),
            1'000U));
    CHECK(
        definition->roles[4].entityId ==
        NetEntityId::Compose(
            hostRuntime.LocalEntityId().Epoch(),
            1'001U));
    CHECK(definition->roles[4].modelHash == 0xAABB2000U);
    CHECK(definition->roles[4].bindingFlags == 0x40U);
    CHECK((definition->roles[4].flags & kRequired) != 0U);
    CHECK(HasLog(hostFacade, "optional-unbound=1"));
    CHECK(HasLog(hostFacade, "unresolved-required=0"));
    const auto expectedFingerprint =
        ComputeAnimSceneDefinitionFingerprint(*definition);
    CHECK(definition->fingerprintLow == expectedFingerprint.low);
    CHECK(definition->fingerprintHigh == expectedFingerprint.high);

    const auto spawnFrame = std::find_if(
        hostTransport.sent.begin(),
        hostTransport.sent.end(),
        [](const Frame& frame) {
            return frame.header.type == MessageType::EntitySpawn;
        });
    const auto definitionFrame = std::find_if(
        hostTransport.sent.begin(),
        hostTransport.sent.end(),
        [](const Frame& frame) {
            return frame.header.type == MessageType::AnimSceneDefinition;
        });
    CHECK(spawnFrame != hostTransport.sent.end());
    CHECK(definitionFrame != hostTransport.sent.end());
    CHECK(spawnFrame < definitionFrame);
    const auto spawnedActor =
        DecodeWorldEntityState(spawnFrame->payload);
    CHECK(spawnedActor.has_value());
    CHECK(
        spawnedActor->entityId ==
        definition->roles[3].entityId);

    const auto queueGuestPresentation = [](
        TestTransport& transport,
        TestFacade& facade,
        const AnimSceneDefinitionPayload& sceneDefinition) {
        constexpr auto kMissionAndAnchor =
            static_cast<std::uint8_t>(
                MissionStateFlag::MissionActive) |
            static_cast<std::uint8_t>(
                MissionStateFlag::AnchorValid);
        Frame mission;
        mission.header.type = MessageType::MissionState;
        mission.header.sequence = ++transport.inboundSequence;
        mission.header.tick = facade.tick;
        mission.payload = EncodeMissionState(
            MissionStatePayload{
                sceneDefinition.hostEntityId,
                sceneDefinition.missionEpoch,
                1U,
                1U,
                MissionPhase::Cutscene,
                kMissionAndAnchor,
                {10.0F, 20.0F, 30.0F},
                90.0F});
        transport.inbound.push_back(std::move(mission));

        Frame cinematic;
        cinematic.header.type = MessageType::MissionCinematicState;
        cinematic.header.sequence = ++transport.inboundSequence;
        cinematic.header.tick = facade.tick;
        cinematic.payload = EncodeMissionCinematicState(
            MissionCinematicStatePayload{
                sceneDefinition.hostEntityId,
                sceneDefinition.missionEpoch,
                sceneDefinition.cinematicGeneration,
                1U,
                1U,
                MissionCinematicPhase::Playing,
                static_cast<std::uint16_t>(
                    MissionCinematicStateFlag::CameraExpected),
                {},
                0.0F});
        transport.inbound.push_back(std::move(cinematic));
    };
    const auto queueDefinition = [](
        TestTransport& transport,
        TestFacade& facade,
        const AnimSceneDefinitionPayload& sceneDefinition) {
        Frame frame;
        frame.header.type = MessageType::AnimSceneDefinition;
        frame.header.sequence = ++transport.inboundSequence;
        frame.header.tick = facade.tick;
        frame.payload = EncodeAnimSceneDefinition(sceneDefinition);
        transport.inbound.push_back(std::move(frame));
    };
    const auto queueRemotePlayerState = [](
        TestTransport& transport,
        TestFacade& facade) {
        Frame frame;
        frame.header.type = MessageType::PlayerState;
        frame.header.sequence = ++transport.inboundSequence;
        frame.header.tick = facade.tick;
        frame.payload = EncodePlayerState(
            PlayerStatePayload{
                transport.remoteEntityId,
                transport.remoteSlot,
                PlayerLifecycle::Alive,
                transport.remotePosition,
                {1.0F, 0.0F, 0.0F},
                transport.remoteHeading,
                1.0F,
                0U,
                {},
                0U,
                0.0F,
                0.0F,
                0.0F,
                0.0F,
                transport.remoteLocomotionEpoch});
        transport.inbound.push_back(std::move(frame));
    };

    TestFacade guestFacade;
    guestFacade.replicatedAnimScenePrepareResult = {
        ReplicatedAnimScenePrepareStatus::Ready,
        3U,
        3U,
        true,
        true,
        ReplicatedAnimScenePrepareStage::Ready};
    TestTransport guestTransport;
    guestTransport.remoteEntityId = definition->hostEntityId;
    BridgeRuntime guestRuntime{guestFacade, guestTransport};
    CHECK(guestRuntime.Start(supported, error));
    guestRuntime.Tick();
    guestTransport.sent.clear();
    queueGuestPresentation(
        guestTransport,
        guestFacade,
        *definition);
    queueDefinition(
        guestTransport,
        guestFacade,
        *definition);
    guestRuntime.Tick();
    CHECK(guestFacade.replicatedAnimScenePrepareCalls == 1U);
    CHECK(HasLog(guestFacade, "PREPARE_PROGRESS] stage=ready"));
    // Standard skip is global to the RDR2 process. While an exact guest scene
    // is preparing/committed it must not be used for delayed-Story suppression,
    // otherwise it would immediately skip the bridge-owned scene too. The
    // terminal grace becomes active again after exact teardown.
    CHECK(!guestFacade.cutsceneSkipInputActive);
    CHECK(guestFacade.lastPreparedAnimSceneDefinition == definition);
    CHECK(
        guestFacade.lastPreparedAnimSceneLocalEntityId ==
        guestRuntime.LocalEntityId());
    const auto ready = LastSentAnimSceneControl(
        guestTransport,
        AnimSceneControlKind::GuestReady);
    CHECK(ready.has_value());
    CHECK(ready->fingerprintLow == definition->fingerprintLow);
    CHECK(ready->fingerprintHigh == definition->fingerprintHigh);
    CHECK(
        (ready->flags & static_cast<std::uint32_t>(
                            AnimSceneControlFlag::CacheHit)) != 0U);

    const auto readyFrame = std::find_if(
        guestTransport.sent.begin(),
        guestTransport.sent.end(),
        [](const Frame& frame) {
            if (frame.header.type != MessageType::AnimSceneControl) {
                return false;
            }
            const auto control = DecodeAnimSceneControl(frame.payload);
            return control.has_value() &&
                   control->kind == AnimSceneControlKind::GuestReady;
        });
    CHECK(readyFrame != guestTransport.sent.end());
    const Frame initialReadyFrame = *readyFrame;
    const auto hostAbortCountBeforeReplyGrace = std::count_if(
        hostTransport.sent.begin(),
        hostTransport.sent.end(),
        [](const Frame& frame) {
            if (frame.header.type != MessageType::AnimSceneControl) {
                return false;
            }
            const auto control = DecodeAnimSceneControl(frame.payload);
            return control.has_value() &&
                   control->kind == AnimSceneControlKind::HostAbort;
        });
    // A guest may finish its legal 2500 ms local prepare window near the
    // boundary. The host keeps 1500 ms of transport grace instead of racing
    // that valid Ready with a symmetric timeout.
    hostFacade.tick += 2'501U;
    hostRuntime.Tick();
    const auto hostAbortCountAfterReplyGrace = std::count_if(
        hostTransport.sent.begin(),
        hostTransport.sent.end(),
        [](const Frame& frame) {
            if (frame.header.type != MessageType::AnimSceneControl) {
                return false;
            }
            const auto control = DecodeAnimSceneControl(frame.payload);
            return control.has_value() &&
                   control->kind == AnimSceneControlKind::HostAbort;
        });
    CHECK(hostAbortCountAfterReplyGrace == hostAbortCountBeforeReplyGrace);

    // A sidecar may replay the same-role HelloAck on an already-authenticated
    // pipe. It is not a reconnect (no SendHello preceded it), so it must not
    // clear the established guest identity or any active hybrid state. A
    // subsequent PlayerState for that same guest keeps the authenticated
    // stream alive, and its GuestReady must still authorize the host commit.
    const auto commandsBeforeDuplicateAck =
        hostFacade.networkCommands.size();
    Frame duplicateHostAcknowledgement;
    duplicateHostAcknowledgement.header.type = MessageType::HelloAck;
    duplicateHostAcknowledgement.header.sequence =
        ++hostTransport.inboundSequence;
    duplicateHostAcknowledgement.header.tick = hostFacade.tick;
    duplicateHostAcknowledgement.payload = {
        static_cast<std::uint8_t>(PlayerSlot::Host)};
    hostTransport.inbound.push_back(
        std::move(duplicateHostAcknowledgement));
    queueRemotePlayerState(hostTransport, hostFacade);
    hostFacade.tick += 1U;
    hostRuntime.Tick();
    CHECK(
        hostFacade.networkCommands.size() ==
        commandsBeforeDuplicateAck);
    CHECK(HasLog(
        hostFacade,
        "duplicate same-role pipe HelloAck ignored"));

    hostTransport.inbound.push_back(initialReadyFrame);
    hostFacade.tick += 1U;
    hostRuntime.Tick();
    const auto commit = LastSentAnimSceneControl(
        hostTransport,
        AnimSceneControlKind::HostPlayCommit);
    CHECK(commit.has_value());
    CHECK(commit->fingerprintLow == definition->fingerprintLow);
    CHECK(commit->fingerprintHigh == definition->fingerprintHigh);
    CHECK(commit->playAtHostTick != 0U);
    CHECK(!hostFacade.hostAnimSceneStartBarrierActive);
    CHECK(HasLog(hostFacade, "PRELOAD_BARRIER] released"));

    Frame commitFrame;
    commitFrame.header.type = MessageType::AnimSceneControl;
    commitFrame.header.sequence = ++guestTransport.inboundSequence;
    commitFrame.header.tick = hostFacade.tick;
    commitFrame.payload = EncodeAnimSceneControl(*commit);
    guestTransport.inbound.push_back(std::move(commitFrame));
    guestFacade.tick += 1U;
    guestRuntime.Tick();
    CHECK(guestFacade.replicatedAnimSceneCommitCalls == 1U);
    CHECK(guestFacade.lastReplicatedAnimSceneCommit == commit);

    // Reconnect only the guest game pipe while the host and its prepare
    // attempt remain live. The sidecar replays the same cached Definition,
    // so the guest's repeated Ready must use a newer action ID and cause an
    // idempotent Commit resend without waiting for stream timeout/rekey.
    const auto hostCommitCountBeforeGuestPipeReconnect = std::count_if(
        hostTransport.sent.begin(),
        hostTransport.sent.end(),
        [](const Frame& frame) {
            if (frame.header.type != MessageType::AnimSceneControl) {
                return false;
            }
            const auto control = DecodeAnimSceneControl(frame.payload);
            return control.has_value() &&
                   control->kind ==
                       AnimSceneControlKind::HostPlayCommit;
        });
    const auto guestSentBeforeOwnPipeReconnect =
        guestTransport.sent.size();
    const auto guestPrepareBeforeOwnPipeReconnect =
        guestFacade.replicatedAnimScenePrepareCalls;
    guestTransport.connected = false;
    guestFacade.tick += 1'001U;
    guestRuntime.Tick();
    CHECK(guestTransport.connected);
    Frame guestPipeResync;
    guestPipeResync.header.type = MessageType::ResyncRequest;
    guestPipeResync.header.sequence = ++guestTransport.inboundSequence;
    guestPipeResync.header.tick = guestFacade.tick;
    guestTransport.inbound.push_back(std::move(guestPipeResync));
    queueRemotePlayerState(guestTransport, guestFacade);
    queueGuestPresentation(
        guestTransport,
        guestFacade,
        *definition);
    queueDefinition(
        guestTransport,
        guestFacade,
        *definition);
    guestFacade.tick += 1U;
    guestRuntime.Tick();
    CHECK(
        guestFacade.replicatedAnimScenePrepareCalls >
        guestPrepareBeforeOwnPipeReconnect);
    const auto guestReconnectReadyFrame = std::find_if(
        guestTransport.sent.begin() +
            static_cast<std::ptrdiff_t>(
                guestSentBeforeOwnPipeReconnect),
        guestTransport.sent.end(),
        [](const Frame& frame) {
            if (frame.header.type != MessageType::AnimSceneControl) {
                return false;
            }
            const auto control = DecodeAnimSceneControl(frame.payload);
            return control.has_value() &&
                   control->kind == AnimSceneControlKind::GuestReady;
        });
    CHECK(guestReconnectReadyFrame != guestTransport.sent.end());
    const auto guestReconnectReady = DecodeAnimSceneControl(
        guestReconnectReadyFrame->payload);
    CHECK(guestReconnectReady.has_value());
    CHECK(
        static_cast<std::int32_t>(
            guestReconnectReady->actionId - ready->actionId) > 0);
    hostTransport.inbound.push_back(*guestReconnectReadyFrame);
    hostFacade.tick += 1U;
    hostRuntime.Tick();
    const auto hostCommitCountAfterGuestPipeReconnect = std::count_if(
        hostTransport.sent.begin(),
        hostTransport.sent.end(),
        [](const Frame& frame) {
            if (frame.header.type != MessageType::AnimSceneControl) {
                return false;
            }
            const auto control = DecodeAnimSceneControl(frame.payload);
            return control.has_value() &&
                   control->kind ==
                       AnimSceneControlKind::HostPlayCommit;
        });
    CHECK(
        hostCommitCountAfterGuestPipeReconnect >
        hostCommitCountBeforeGuestPipeReconnect);

    const auto hostCommitCountBeforeResync = std::count_if(
        hostTransport.sent.begin(),
        hostTransport.sent.end(),
        [](const Frame& frame) {
            if (frame.header.type != MessageType::AnimSceneControl) {
                return false;
            }
            const auto control = DecodeAnimSceneControl(frame.payload);
            return control.has_value() &&
                   control->kind ==
                       AnimSceneControlKind::HostPlayCommit;
        });
    const auto hostSentBeforeResyncRecovery = hostTransport.sent.size();
    Frame resync;
    resync.header.type = MessageType::ResyncRequest;
    resync.header.sequence = ++hostTransport.inboundSequence;
    resync.header.tick = hostFacade.tick;
    hostTransport.inbound.push_back(std::move(resync));
    // Exact production race: both frames are returned by one Poll, with the
    // replacement player stream immediately following ResyncRequest.
    queueRemotePlayerState(hostTransport, hostFacade);
    hostFacade.tick += 1U;
    hostRuntime.Tick();
    CHECK(HasLog(hostFacade, "retained the active host world graph"));
    const auto resyncRecoveryBegin =
        hostTransport.sent.begin() +
        static_cast<std::ptrdiff_t>(hostSentBeforeResyncRecovery);
    const auto resyncSpawnFrame = std::find_if(
        resyncRecoveryBegin,
        hostTransport.sent.end(),
        [](const Frame& frame) {
            return frame.header.type == MessageType::EntitySpawn;
        });
    const auto resyncDefinitionFrame = std::find_if(
        resyncRecoveryBegin,
        hostTransport.sent.end(),
        [](const Frame& frame) {
            return frame.header.type == MessageType::AnimSceneDefinition;
        });
    const auto resyncReplicaStateFrame = std::find_if(
        resyncRecoveryBegin,
        hostTransport.sent.end(),
        [](const Frame& frame) {
            return frame.header.type ==
                   MessageType::AnimSceneReplicaState;
        });
    const auto resyncMissionFrame = std::find_if(
        resyncRecoveryBegin,
        hostTransport.sent.end(),
        [](const Frame& frame) {
            return frame.header.type == MessageType::MissionState;
        });
    const auto resyncCinematicFrame = std::find_if(
        resyncRecoveryBegin,
        hostTransport.sent.end(),
        [](const Frame& frame) {
            return frame.header.type ==
                   MessageType::MissionCinematicState;
        });
    CHECK(resyncMissionFrame != hostTransport.sent.end());
    CHECK(resyncCinematicFrame != hostTransport.sent.end());
    CHECK(resyncSpawnFrame != hostTransport.sent.end());
    CHECK(resyncDefinitionFrame != hostTransport.sent.end());
    CHECK(resyncReplicaStateFrame != hostTransport.sent.end());
    CHECK(resyncMissionFrame < resyncCinematicFrame);
    CHECK(resyncCinematicFrame < resyncSpawnFrame);
    CHECK(resyncSpawnFrame < resyncDefinitionFrame);
    const auto resyncDefinition =
        DecodeAnimSceneDefinition(resyncDefinitionFrame->payload);
    const auto resyncReplicaState =
        DecodeAnimSceneReplicaState(resyncReplicaStateFrame->payload);
    CHECK(resyncDefinition.has_value());
    CHECK(resyncReplicaState.has_value());
    CHECK(
        resyncDefinition->definitionRevision >
        definition->definitionRevision);
    CHECK(
        resyncDefinition->fingerprintLow != definition->fingerprintLow ||
        resyncDefinition->fingerprintHigh != definition->fingerprintHigh);
    CHECK(
        resyncReplicaState->definitionRevision ==
        resyncDefinition->definitionRevision);

    const auto guestSentBeforeResyncRecovery = guestTransport.sent.size();
    guestTransport.inbound.push_back(*resyncDefinitionFrame);
    guestFacade.tick += 1U;
    guestRuntime.Tick();
    const auto resyncReadyFrame = std::find_if(
        guestTransport.sent.begin() +
            static_cast<std::ptrdiff_t>(guestSentBeforeResyncRecovery),
        guestTransport.sent.end(),
        [](const Frame& frame) {
            if (frame.header.type != MessageType::AnimSceneControl) {
                return false;
            }
            const auto control = DecodeAnimSceneControl(frame.payload);
            return control.has_value() &&
                   control->kind == AnimSceneControlKind::GuestReady;
        });
    CHECK(resyncReadyFrame != guestTransport.sent.end());
    hostTransport.inbound.push_back(*resyncReadyFrame);
    hostFacade.tick += 1U;
    hostRuntime.Tick();
    const auto hostCommitCountAfterResync = std::count_if(
        hostTransport.sent.begin(),
        hostTransport.sent.end(),
        [](const Frame& frame) {
            if (frame.header.type != MessageType::AnimSceneControl) {
                return false;
            }
            const auto control = DecodeAnimSceneControl(frame.payload);
            return control.has_value() &&
                   control->kind ==
                       AnimSceneControlKind::HostPlayCommit;
        });
    CHECK(hostCommitCountAfterResync > hostCommitCountBeforeResync);

    const auto worldDespawnsBeforePipeReconnect = std::count_if(
        hostTransport.sent.begin(),
        hostTransport.sent.end(),
        [](const Frame& frame) {
            return frame.header.type == MessageType::EntityDespawn;
        });
    const auto commitsBeforePipeReconnect = hostCommitCountAfterResync;
    const auto sentBeforePipeReconnect = hostTransport.sent.size();
    hostTransport.connected = false;
    hostFacade.tick += 1'001U;
    hostRuntime.Tick();
    CHECK(hostTransport.connected);
    CHECK(HasLog(hostFacade, "retaining active host definition"));
    CHECK(HasLog(hostFacade, "stable world spawns"));
    const auto reconnectBegin =
        hostTransport.sent.begin() +
        static_cast<std::ptrdiff_t>(sentBeforePipeReconnect);
    const auto replayedSpawnFrame = std::find_if(
        reconnectBegin,
        hostTransport.sent.end(),
        [](const Frame& frame) {
            return frame.header.type == MessageType::EntitySpawn;
        });
    const auto replayedDefinitionFrame = std::find_if(
        reconnectBegin,
        hostTransport.sent.end(),
        [](const Frame& frame) {
            return frame.header.type == MessageType::AnimSceneDefinition;
        });
    CHECK(replayedSpawnFrame != hostTransport.sent.end());
    CHECK(replayedDefinitionFrame != hostTransport.sent.end());
    CHECK(replayedSpawnFrame < replayedDefinitionFrame);
    const auto replayedDefinition =
        LastSentAnimSceneDefinition(hostTransport);
    CHECK(replayedDefinition.has_value());
    CHECK(
        replayedDefinition->definitionRevision >
        resyncDefinition->definitionRevision);
    CHECK(
        replayedDefinition->fingerprintLow !=
            resyncDefinition->fingerprintLow ||
        replayedDefinition->fingerprintHigh !=
            resyncDefinition->fingerprintHigh);
    CHECK(
        replayedDefinition->roles[2].entityId ==
        definition->roles[2].entityId);
    const auto worldDespawnsAfterPipeReconnect = std::count_if(
        hostTransport.sent.begin(),
        hostTransport.sent.end(),
        [](const Frame& frame) {
            return frame.header.type == MessageType::EntityDespawn;
        });
    CHECK(
        worldDespawnsAfterPipeReconnect ==
        worldDespawnsBeforePipeReconnect);

    const auto guestPrepareCallsBeforePipeReconnect =
        guestFacade.replicatedAnimScenePrepareCalls;
    const auto guestSentBeforePipeReconnect = guestTransport.sent.size();
    guestTransport.inbound.push_back(*replayedSpawnFrame);
    guestTransport.inbound.push_back(*replayedDefinitionFrame);
    guestFacade.tick += 1U;
    guestRuntime.Tick();
    CHECK(
        guestFacade.replicatedAnimScenePrepareCalls >
        guestPrepareCallsBeforePipeReconnect);
    const auto pipeReadyFrame = std::find_if(
        guestTransport.sent.begin() +
            static_cast<std::ptrdiff_t>(guestSentBeforePipeReconnect),
        guestTransport.sent.end(),
        [](const Frame& frame) {
            if (frame.header.type != MessageType::AnimSceneControl) {
                return false;
            }
            const auto control = DecodeAnimSceneControl(frame.payload);
            return control.has_value() &&
                   control->kind == AnimSceneControlKind::GuestReady;
        });
    CHECK(pipeReadyFrame != guestTransport.sent.end());
    hostTransport.inbound.push_back(*pipeReadyFrame);
    hostFacade.tick += 1U;
    hostRuntime.Tick();
    const auto commitsAfterPipeReconnect = std::count_if(
        hostTransport.sent.begin(),
        hostTransport.sent.end(),
        [](const Frame& frame) {
            if (frame.header.type != MessageType::AnimSceneControl) {
                return false;
            }
            const auto control = DecodeAnimSceneControl(frame.payload);
            return control.has_value() &&
                   control->kind ==
                       AnimSceneControlKind::HostPlayCommit;
        });
    CHECK(commitsAfterPipeReconnect > commitsBeforePipeReconnect);

    const auto worldDespawnsBeforeStreamTimeout =
        worldDespawnsAfterPipeReconnect;
    const auto hostCommitsBeforeStreamTimeout =
        commitsAfterPipeReconnect;
    const auto guestPrepareCallsBeforeStreamTimeout =
        guestFacade.replicatedAnimScenePrepareCalls;
    const auto guestCommitCallsBeforeStreamTimeout =
        guestFacade.replicatedAnimSceneCommitCalls;
    hostFacade.tick += 5'001U;
    hostRuntime.Tick();
    CHECK(HasLog(hostFacade, "STREAM_TIMEOUT"));
    const auto worldDespawnsAfterStreamTimeout = std::count_if(
        hostTransport.sent.begin(),
        hostTransport.sent.end(),
        [](const Frame& frame) {
            return frame.header.type == MessageType::EntityDespawn;
        });
    CHECK(
        worldDespawnsAfterStreamTimeout ==
        worldDespawnsBeforeStreamTimeout);

    // The guest independently loses the host stream and discards its prepared
    // native handle/world graph. Reproduce the critical batch order exactly:
    // a fresh PlayerState followed by a delayed pre-timeout Ready in the same
    // Poll. Authentication is fresh, but the old prepare attempt must remain
    // invalid until stable spawns and the new definition revision are sent.
    guestFacade.tick += 5'001U;
    guestRuntime.Tick();
    CHECK(HasLog(guestFacade, "remote stream timed out"));
    const auto hostSentBeforeStreamRecovery = hostTransport.sent.size();
    queueRemotePlayerState(hostTransport, hostFacade);
    hostTransport.inbound.push_back(initialReadyFrame);
    hostFacade.tick += 1U;
    hostRuntime.Tick();
    const auto commitsAfterStaleReady = std::count_if(
        hostTransport.sent.begin(),
        hostTransport.sent.end(),
        [](const Frame& frame) {
            if (frame.header.type != MessageType::AnimSceneControl) {
                return false;
            }
            const auto control = DecodeAnimSceneControl(frame.payload);
            return control.has_value() &&
                   control->kind ==
                       AnimSceneControlKind::HostPlayCommit;
        });
    CHECK(commitsAfterStaleReady == hostCommitsBeforeStreamTimeout);
    CHECK(HasLog(
        hostFacade,
        "rejected guest reply before stable world spawns and the current definition revision were sent"));

    // Restore both player streams. The host must replay every stable spawn
    // before replaying the cached definition; the reset guest then performs a
    // fresh prepare and only that new Ready may produce another Commit.
    const auto recoveryBegin =
        hostTransport.sent.begin() +
        static_cast<std::ptrdiff_t>(hostSentBeforeStreamRecovery);
    const auto recoveredSpawnFrame = std::find_if(
        recoveryBegin,
        hostTransport.sent.end(),
        [](const Frame& frame) {
            return frame.header.type == MessageType::EntitySpawn;
        });
    const auto recoveredDefinitionFrame = std::find_if(
        recoveryBegin,
        hostTransport.sent.end(),
        [](const Frame& frame) {
            return frame.header.type == MessageType::AnimSceneDefinition;
        });
    const auto recoveredMissionFrame = std::find_if(
        recoveryBegin,
        hostTransport.sent.end(),
        [](const Frame& frame) {
            return frame.header.type == MessageType::MissionState;
        });
    const auto recoveredCinematicFrame = std::find_if(
        recoveryBegin,
        hostTransport.sent.end(),
        [](const Frame& frame) {
            return frame.header.type ==
                   MessageType::MissionCinematicState;
        });
    const auto recoveredReplicaStateFrame = std::find_if(
        recoveryBegin,
        hostTransport.sent.end(),
        [](const Frame& frame) {
            return frame.header.type ==
                   MessageType::AnimSceneReplicaState;
        });
    CHECK(recoveredMissionFrame != hostTransport.sent.end());
    CHECK(recoveredCinematicFrame != hostTransport.sent.end());
    CHECK(recoveredReplicaStateFrame != hostTransport.sent.end());
    const auto recoveredCinematicState =
        DecodeMissionCinematicState(recoveredCinematicFrame->payload);
    CHECK(recoveredCinematicState.has_value());
    const auto recoveredCinematicFrameSequence =
        recoveredCinematicFrame->header.sequence;
    CHECK(recoveredSpawnFrame != hostTransport.sent.end());
    CHECK(recoveredDefinitionFrame != hostTransport.sent.end());
    CHECK(recoveredMissionFrame < recoveredSpawnFrame);
    CHECK(recoveredCinematicFrame < recoveredSpawnFrame);
    CHECK(recoveredSpawnFrame < recoveredDefinitionFrame);
    const auto recoveredDefinition =
        DecodeAnimSceneDefinition(recoveredDefinitionFrame->payload);
    const auto recoveredReplicaState =
        DecodeAnimSceneReplicaState(recoveredReplicaStateFrame->payload);
    CHECK(recoveredDefinition.has_value());
    CHECK(recoveredReplicaState.has_value());
    CHECK(
        recoveredDefinition->definitionRevision >
        replayedDefinition->definitionRevision);
    CHECK(
        recoveredDefinition->fingerprintLow !=
            replayedDefinition->fingerprintLow ||
        recoveredDefinition->fingerprintHigh !=
            replayedDefinition->fingerprintHigh);
    CHECK(
        recoveredReplicaState->definitionRevision ==
        recoveredDefinition->definitionRevision);
    const auto recoveredActor =
        DecodeWorldEntityState(recoveredSpawnFrame->payload);
    CHECK(recoveredActor.has_value());
    CHECK(
        recoveredActor->entityId ==
        definition->roles[3].entityId);

    const auto guestSentBeforeStreamRecovery = guestTransport.sent.size();
    const auto guestWorldSpawnsBeforeStreamRecovery =
        guestFacade.worldEntitySpawns.size();
    queueRemotePlayerState(guestTransport, guestFacade);
    for (auto iterator = recoveryBegin;
         iterator != hostTransport.sent.end() &&
         iterator <= recoveredDefinitionFrame;
         ++iterator) {
        if (iterator->header.type == MessageType::MissionState ||
            iterator->header.type ==
                MessageType::MissionCinematicState ||
            iterator->header.type == MessageType::EntitySpawn ||
            iterator->header.type == MessageType::AnimSceneDefinition) {
            guestTransport.inbound.push_back(*iterator);
        }
    }
    guestFacade.tick += 1U;
    guestRuntime.Tick();
    CHECK(
        guestFacade.worldEntitySpawns.size() >
        guestWorldSpawnsBeforeStreamRecovery);
    CHECK(
        guestFacade.replicatedAnimScenePrepareCalls >
        guestPrepareCallsBeforeStreamTimeout);
    const auto freshReadyFrame = std::find_if(
        guestTransport.sent.begin() +
            static_cast<std::ptrdiff_t>(guestSentBeforeStreamRecovery),
        guestTransport.sent.end(),
        [](const Frame& frame) {
            if (frame.header.type != MessageType::AnimSceneControl) {
                return false;
            }
            const auto control = DecodeAnimSceneControl(frame.payload);
            return control.has_value() &&
                   control->kind == AnimSceneControlKind::GuestReady;
        });
    CHECK(freshReadyFrame != guestTransport.sent.end());

    const auto hostSentBeforeFreshReady = hostTransport.sent.size();
    hostTransport.inbound.push_back(*freshReadyFrame);
    hostFacade.tick += 1U;
    hostRuntime.Tick();
    const auto freshCommitFrame = std::find_if(
        hostTransport.sent.begin() +
            static_cast<std::ptrdiff_t>(hostSentBeforeFreshReady),
        hostTransport.sent.end(),
        [](const Frame& frame) {
            if (frame.header.type != MessageType::AnimSceneControl) {
                return false;
            }
            const auto control = DecodeAnimSceneControl(frame.payload);
            return control.has_value() &&
                   control->kind ==
                       AnimSceneControlKind::HostPlayCommit;
        });
    CHECK(freshCommitFrame != hostTransport.sent.end());
    const auto commitsAfterStreamTimeout = std::count_if(
        hostTransport.sent.begin(),
        hostTransport.sent.end(),
        [](const Frame& frame) {
            if (frame.header.type != MessageType::AnimSceneControl) {
                return false;
            }
            const auto control = DecodeAnimSceneControl(frame.payload);
            return control.has_value() &&
                   control->kind ==
                       AnimSceneControlKind::HostPlayCommit;
        });
    CHECK(commitsAfterStreamTimeout > commitsAfterStaleReady);
    guestTransport.inbound.push_back(*freshCommitFrame);
    guestFacade.tick += 1U;
    guestRuntime.Tick();
    CHECK(
        guestFacade.replicatedAnimSceneCommitCalls >
        guestCommitCallsBeforeStreamTimeout);

    // Restore a live remote participant, enter the one-way PrepareResume
    // barrier, then repeat the full pipe Hello renegotiation. No call may
    // lower the barrier before the guest supplies ResumeReady.
    hostTransport.connected = false;
    hostFacade.tick += 1'001U;
    hostRuntime.Tick();
    hostFacade.sample.cutsceneActive = false;
    hostFacade.tick += 1U;
    hostRuntime.Tick();
    hostFacade.tick += 751U;
    hostRuntime.Tick();
    const auto prepareResumeBeforeReconnect =
        LastSentMissionCinematicState(hostTransport);
    CHECK(prepareResumeBeforeReconnect.has_value());
    CHECK(
        prepareResumeBeforeReconnect->phase ==
        MissionCinematicPhase::PrepareResume);
    CHECK(hostFacade.missionResumeBarrierActive);
    const auto barrierHistoryBeforeReconnect =
        hostFacade.missionResumeBarrierStates.size();
    hostTransport.injectRemoteState = false;
    hostTransport.connected = false;
    hostFacade.tick += 1'001U;
    hostRuntime.Tick();
    CHECK(hostFacade.missionResumeBarrierActive);
    CHECK(std::all_of(
        hostFacade.missionResumeBarrierStates.begin() +
            static_cast<std::ptrdiff_t>(barrierHistoryBeforeReconnect),
        hostFacade.missionResumeBarrierStates.end(),
        [](const bool active) { return active; }));
    const auto prepareResumeAfterReconnect =
        LastSentMissionCinematicState(hostTransport);
    CHECK(prepareResumeAfterReconnect.has_value());
    CHECK(
        prepareResumeAfterReconnect->phase ==
        MissionCinematicPhase::PrepareResume);
    // Real sidecars may deliver HelloAck one game tick before the first LAN
    // PlayerState. That gap must not be interpreted as a departed guest.
    queueRemotePlayerState(hostTransport, hostFacade);
    hostFacade.tick += 1U;
    hostRuntime.Tick();
    CHECK(hostFacade.missionResumeBarrierActive);
    CHECK(std::all_of(
        hostFacade.missionResumeBarrierStates.begin() +
            static_cast<std::ptrdiff_t>(barrierHistoryBeforeReconnect),
        hostFacade.missionResumeBarrierStates.end(),
        [](const bool active) { return active; }));
    const auto prepareResumeAfterDelayedPlayerState =
        LastSentMissionCinematicState(hostTransport);
    CHECK(prepareResumeAfterDelayedPlayerState.has_value());
    CHECK(
        prepareResumeAfterDelayedPlayerState->phase ==
        MissionCinematicPhase::PrepareResume);
    CHECK(HasLog(
        hostFacade,
        "fresh authenticated guest stream restored"));
    hostTransport.injectRemoteState = true;

    const auto barrierHistoryBeforeMissingAck =
        hostFacade.missionResumeBarrierStates.size();
    hostTransport.acknowledgeHello = false;
    hostTransport.connected = false;
    hostFacade.tick += 1'001U;
    hostRuntime.Tick();
    CHECK(hostTransport.connected);
    CHECK(hostFacade.missionResumeBarrierActive);
    // The pipe dies before HelloAck. A second retry must retain the pending
    // pre-Hello Host authority, exact definition and PrepareResume barrier.
    hostTransport.acknowledgeHello = true;
    hostTransport.connected = false;
    hostFacade.tick += 1'001U;
    hostRuntime.Tick();
    CHECK(hostTransport.connected);
    CHECK(hostFacade.missionResumeBarrierActive);
    CHECK(std::all_of(
        hostFacade.missionResumeBarrierStates.begin() +
            static_cast<std::ptrdiff_t>(barrierHistoryBeforeMissingAck),
        hostFacade.missionResumeBarrierStates.end(),
        [](const bool active) { return active; }));
    const auto prepareResumeAfterMissingAckRetry =
        LastSentMissionCinematicState(hostTransport);
    CHECK(prepareResumeAfterMissingAckRetry.has_value());
    CHECK(
        prepareResumeAfterMissingAckRetry->phase ==
        MissionCinematicPhase::PrepareResume);

    hostTransport.acknowledgementPayload = {
        static_cast<std::uint8_t>(PlayerSlot::Guest)};
    hostTransport.connected = false;
    hostFacade.tick += 1'001U;
    hostRuntime.Tick();
    CHECK(!hostTransport.connected);
    CHECK(!hostFacade.missionResumeBarrierActive);
    CHECK(HasLog(
        hostFacade,
        "conflicting pipe HelloAck role after reconnect"));

    // SAFE_FALLBACK has no exact AnimSceneDefinition to retain, but its
    // mission FSM owns the same one-way PrepareResume barrier. A pipe Hello
    // renegotiation must preserve that barrier and cinematic generation until
    // ResumeReady just like the exact-definition path above.
    TestFacade fallbackFacade;
    fallbackFacade.sample.localHandle = kHostPlayerHandle;
    fallbackFacade.sample.position = {10.0F, 20.0F, 30.0F};
    fallbackFacade.sampledAnimScene = sampledScene;
    fallbackFacade.sampledAnimSceneLocalHandle = 88;
    TestTransport fallbackTransport;
    fallbackTransport.acknowledgementPayload = {
        static_cast<std::uint8_t>(PlayerSlot::Host)};
    fallbackTransport.remoteSlot = PlayerSlot::Guest;
    BridgeRuntime fallbackRuntime{fallbackFacade, fallbackTransport};
    CHECK(fallbackRuntime.Start(supported, error));
    fallbackRuntime.Tick();
    fallbackTransport.sent.clear();
    fallbackFacade.sample.missionActive = true;
    fallbackFacade.sample.cutsceneActive = true;
    fallbackFacade.tick += 500U;
    fallbackRuntime.Tick();
    CHECK(!LastSentAnimSceneDefinition(fallbackTransport).has_value());
    fallbackFacade.sample.cutsceneActive = false;
    fallbackFacade.tick += 1U;
    fallbackRuntime.Tick();
    fallbackFacade.tick += 751U;
    fallbackRuntime.Tick();
    const auto fallbackPrepareBeforeReconnect =
        LastSentMissionCinematicState(fallbackTransport);
    CHECK(fallbackPrepareBeforeReconnect.has_value());
    CHECK(
        fallbackPrepareBeforeReconnect->phase ==
        MissionCinematicPhase::PrepareResume);
    CHECK(fallbackFacade.missionResumeBarrierActive);
    CHECK(!LastSentAnimSceneDefinition(fallbackTransport).has_value());
    const auto fallbackBarrierHistoryBeforeReconnect =
        fallbackFacade.missionResumeBarrierStates.size();
    const auto fallbackGeneration =
        fallbackPrepareBeforeReconnect->cinematicGeneration;
    fallbackTransport.connected = false;
    fallbackFacade.tick += 1'001U;
    fallbackRuntime.Tick();
    CHECK(fallbackTransport.connected);
    CHECK(fallbackFacade.missionResumeBarrierActive);
    CHECK(std::all_of(
        fallbackFacade.missionResumeBarrierStates.begin() +
            static_cast<std::ptrdiff_t>(
                fallbackBarrierHistoryBeforeReconnect),
        fallbackFacade.missionResumeBarrierStates.end(),
        [](const bool active) { return active; }));
    const auto fallbackPrepareAfterReconnect =
        LastSentMissionCinematicState(fallbackTransport);
    CHECK(fallbackPrepareAfterReconnect.has_value());
    CHECK(
        fallbackPrepareAfterReconnect->phase ==
        MissionCinematicPhase::PrepareResume);
    CHECK(
        fallbackPrepareAfterReconnect->cinematicGeneration ==
        fallbackGeneration);
    CHECK(!LastSentAnimSceneDefinition(fallbackTransport).has_value());
    CHECK(HasLog(
        fallbackFacade,
        "retaining SAFE_FALLBACK cinematic generation and resume barrier"));

    const auto abortCallsBeforeTerminal =
        guestFacade.replicatedAnimSceneAbortCalls;
    guestTransport.inboundSequence = std::max(
        guestTransport.inboundSequence,
        recoveredCinematicFrameSequence);
    Frame completed;
    completed.header.type = MessageType::MissionCinematicState;
    completed.header.sequence = ++guestTransport.inboundSequence;
    completed.header.tick = guestFacade.tick;
    completed.payload = EncodeMissionCinematicState(
        MissionCinematicStatePayload{
            definition->hostEntityId,
            definition->missionEpoch,
            definition->cinematicGeneration,
            recoveredCinematicState->revision + 1U,
            1U,
            MissionCinematicPhase::Completed,
            static_cast<std::uint16_t>(
                MissionCinematicStateFlag::AnchorValid),
            {10.0F, 20.0F, 30.0F},
            90.0F});
    guestTransport.inbound.push_back(std::move(completed));
    guestFacade.tick += 1U;
    guestRuntime.Tick();
    CHECK(
        guestFacade.replicatedAnimSceneAbortCalls >
        abortCallsBeforeTerminal);
    const auto commitsBeforeLateDecision =
        guestFacade.replicatedAnimSceneCommitCalls;
    Frame lateCommit;
    lateCommit.header.type = MessageType::AnimSceneControl;
    lateCommit.header.sequence = ++guestTransport.inboundSequence;
    lateCommit.header.tick = guestFacade.tick;
    lateCommit.payload = EncodeAnimSceneControl(*commit);
    guestTransport.inbound.push_back(std::move(lateCommit));
    guestFacade.tick += 1U;
    guestRuntime.Tick();
    CHECK(
        guestFacade.replicatedAnimSceneCommitCalls ==
        commitsBeforeLateDecision);

    TestFacade decisionTimeoutFacade;
    decisionTimeoutFacade.replicatedAnimScenePrepareResult = {
        ReplicatedAnimScenePrepareStatus::Ready,
        3U,
        3U,
        false,
        true,
        ReplicatedAnimScenePrepareStage::Ready};
    TestTransport decisionTimeoutTransport;
    decisionTimeoutTransport.remoteEntityId = definition->hostEntityId;
    BridgeRuntime decisionTimeoutRuntime{
        decisionTimeoutFacade,
        decisionTimeoutTransport};
    CHECK(decisionTimeoutRuntime.Start(supported, error));
    decisionTimeoutRuntime.Tick();
    decisionTimeoutTransport.sent.clear();
    queueGuestPresentation(
        decisionTimeoutTransport,
        decisionTimeoutFacade,
        *definition);
    queueDefinition(
        decisionTimeoutTransport,
        decisionTimeoutFacade,
        *definition);
    decisionTimeoutRuntime.Tick();
    const auto decisionTimeoutAbortBefore =
        decisionTimeoutFacade.replicatedAnimSceneAbortCalls;
    decisionTimeoutFacade.tick += 2'501U;
    decisionTimeoutRuntime.Tick();
    CHECK(
        decisionTimeoutFacade.replicatedAnimSceneAbortCalls ==
        decisionTimeoutAbortBefore);
    CHECK(!HasLog(decisionTimeoutFacade, "DECISION_TIMEOUT"));
    // Keep the authoritative cinematic stream fresh while the guest uses the
    // additional network grace. Mission host-loss has a separate 3000 ms
    // safety window and must not be what ends this exact-path test.
    queueGuestPresentation(
        decisionTimeoutTransport,
        decisionTimeoutFacade,
        *definition);
    decisionTimeoutRuntime.Tick();
    decisionTimeoutFacade.tick += 1'500U;
    decisionTimeoutRuntime.Tick();
    CHECK(
        decisionTimeoutFacade.replicatedAnimSceneAbortCalls >
        decisionTimeoutAbortBefore);
    const auto decisionTimeoutRejection = LastSentAnimSceneControl(
        decisionTimeoutTransport,
        AnimSceneControlKind::GuestRejected);
    CHECK(decisionTimeoutRejection.has_value());
    CHECK(
        decisionTimeoutRejection->reason ==
        AnimSceneControlReason::LoadTimeout);
    CHECK(HasLog(decisionTimeoutFacade, "DECISION_TIMEOUT"));

    const auto verifyRejectedPrepare = [&supported,
                                        &error,
                                        &queueGuestPresentation,
                                        &queueDefinition,
                                        &definition](
                                           const ReplicatedAnimScenePrepareStatus status,
                                           const AnimSceneControlReason expectedReason) {
        TestFacade rejectedFacade;
        rejectedFacade.replicatedAnimScenePrepareResult.status = status;
        TestTransport rejectedTransport;
        rejectedTransport.remoteEntityId = definition->hostEntityId;
        BridgeRuntime rejectedRuntime{
            rejectedFacade,
            rejectedTransport};
        CHECK(rejectedRuntime.Start(supported, error));
        rejectedRuntime.Tick();
        rejectedTransport.sent.clear();
        queueGuestPresentation(
            rejectedTransport,
            rejectedFacade,
            *definition);
        queueDefinition(
            rejectedTransport,
            rejectedFacade,
            *definition);
        rejectedRuntime.Tick();
        CHECK(rejectedFacade.replicatedAnimScenePrepareCalls == 1U);
        CHECK(rejectedFacade.replicatedAnimSceneCommitCalls == 0U);
        const auto rejection = LastSentAnimSceneControl(
            rejectedTransport,
            AnimSceneControlKind::GuestRejected);
        CHECK(rejection.has_value());
        CHECK(rejection->reason == expectedReason);
        CHECK(
            !LastSentAnimSceneControl(
                 rejectedTransport,
                 AnimSceneControlKind::GuestReady)
                 .has_value());
        CHECK(HasLog(rejectedFacade, "SAFE_FALLBACK retained"));
    };
    verifyRejectedPrepare(
        ReplicatedAnimScenePrepareStatus::Unsupported,
        AnimSceneControlReason::UnsupportedResource);
    verifyRejectedPrepare(
        ReplicatedAnimScenePrepareStatus::MissingBinding,
        AnimSceneControlReason::MissingBinding);

    TestFacade staleFacade;
    staleFacade.replicatedAnimScenePrepareResult = {
        ReplicatedAnimScenePrepareStatus::Ready,
        3U,
        3U,
        true,
        false,
        ReplicatedAnimScenePrepareStage::Ready};
    TestTransport staleTransport;
    staleTransport.remoteEntityId = definition->hostEntityId;
    BridgeRuntime staleRuntime{staleFacade, staleTransport};
    CHECK(staleRuntime.Start(supported, error));
    staleRuntime.Tick();
    queueGuestPresentation(
        staleTransport,
        staleFacade,
        *definition);

    Frame badFingerprint;
    badFingerprint.header.type = MessageType::AnimSceneDefinition;
    badFingerprint.header.sequence = ++staleTransport.inboundSequence;
    badFingerprint.header.tick = staleFacade.tick;
    badFingerprint.payload = EncodeAnimSceneDefinition(*definition);
    badFingerprint.payload[24] ^= 0x01U;
    staleTransport.inbound.push_back(std::move(badFingerprint));
    staleRuntime.Tick();
    CHECK(staleFacade.replicatedAnimScenePrepareCalls == 0U);

    auto wrongGeneration = *definition;
    ++wrongGeneration.cinematicGeneration;
    wrongGeneration.fingerprintLow = 0U;
    wrongGeneration.fingerprintHigh = 0U;
    const auto wrongGenerationFingerprint =
        ComputeAnimSceneDefinitionFingerprint(wrongGeneration);
    wrongGeneration.fingerprintLow = wrongGenerationFingerprint.low;
    wrongGeneration.fingerprintHigh = wrongGenerationFingerprint.high;
    queueDefinition(
        staleTransport,
        staleFacade,
        wrongGeneration);
    staleFacade.tick += 1U;
    staleRuntime.Tick();
    CHECK(staleFacade.replicatedAnimScenePrepareCalls == 0U);
    CHECK(staleFacade.replicatedAnimSceneCommitCalls == 0U);
}

void RuntimeGuestPresentsHostObjectiveAndCutsceneCamera() {
    TestFacade facade;
    facade.sample.position = {1.0F, 2.0F, 3.0F};
    TestTransport transport;
    BridgeRuntime runtime{facade, transport};
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;
    CHECK(runtime.Start(supported, error));
    runtime.Tick();
    CHECK(runtime.LocalSlot() == PlayerSlot::Guest);

    constexpr auto kMissionAndAnchor =
        static_cast<std::uint8_t>(
            MissionStateFlag::MissionActive) |
        static_cast<std::uint8_t>(
            MissionStateFlag::AnchorValid);
    const auto pushMission = [&](const MissionPhase phase,
                                 const std::uint32_t revision,
                                 const std::uint8_t flags) {
        Frame frame;
        frame.header.type = MessageType::MissionState;
        frame.header.sequence = ++transport.inboundSequence;
        frame.header.tick = facade.tick;
        frame.payload = EncodeMissionState(
            MissionStatePayload{
                transport.remoteEntityId,
                7U,
                revision,
                1U,
                phase,
                flags,
                {40.0F, 50.0F, 60.0F},
                90.0F});
        transport.inbound.push_back(std::move(frame));
    };
    const auto pushCamera = [&](const NetEntityId hostId,
                                const std::uint32_t revision) {
        Frame frame;
        frame.header.type = MessageType::MissionCameraState;
        frame.header.sequence = ++transport.inboundSequence;
        frame.header.tick = facade.tick;
        frame.payload = EncodeMissionCameraState(
            MissionCameraStatePayload{
                hostId,
                7U,
                1U,
                revision,
                static_cast<std::uint32_t>(
                    MissionCameraStateFlag::Active) |
                    static_cast<std::uint32_t>(
                        MissionCameraStateFlag::SourceRenderingScriptCamera),
                {101.0F, 102.0F, 103.0F},
                {-5.0F, 0.0F, 135.0F},
                55.0F});
        transport.inbound.push_back(std::move(frame));
    };
    const auto pushCinematic = [&] (
        const MissionCinematicPhase phase,
        const std::uint32_t revision,
        const std::uint16_t flags) {
        Frame frame;
        frame.header.type = MessageType::MissionCinematicState;
        frame.header.sequence = ++transport.inboundSequence;
        frame.header.tick = facade.tick;
        frame.payload = EncodeMissionCinematicState(
            MissionCinematicStatePayload{
                transport.remoteEntityId,
                7U,
                1U,
                revision,
                1U,
                phase,
                flags,
                (flags & static_cast<std::uint16_t>(
                              MissionCinematicStateFlag::AnchorValid)) != 0U
                    ? Vec3{40.0F, 50.0F, 60.0F}
                    : Vec3{},
                (flags & static_cast<std::uint16_t>(
                              MissionCinematicStateFlag::AnchorValid)) != 0U
                    ? 90.0F
                    : 0.0F});
        transport.inbound.push_back(std::move(frame));
    };

    pushMission(MissionPhase::Active, 1U, kMissionAndAnchor);
    runtime.Tick();
    CHECK(facade.missionCompanionPresentation.active);
    CHECK(facade.missionCompanionPresentation.liveHostPosition);
    CHECK(
        facade.missionCompanionPresentation.target.x ==
        transport.remotePosition.x);
    CHECK(!facade.replicatedMissionCameraSpectatorActive);

    facade.tick += 50U;
    pushMission(MissionPhase::Cutscene, 2U, kMissionAndAnchor);
    pushCinematic(
        MissionCinematicPhase::Playing,
        1U,
        static_cast<std::uint16_t>(
            MissionCinematicStateFlag::CameraExpected));
    pushCamera(NetEntityId::Compose(0xDEADBEEFU, 1U), 1U);
    pushCamera(transport.remoteEntityId, 2U);
    runtime.Tick();
    CHECK(facade.missionSpectatorActive);
    CHECK(facade.replicatedMissionCameraSpectatorActive);
    CHECK(facade.replicatedMissionCameraState.has_value());
    CHECK(facade.replicatedMissionCameraState->revision == 2U);
    CHECK(!facade.missionCompanionPresentation.active);
    CHECK(HasLog(facade, "mismatched host identity"));
    CHECK(HasLog(facade, "[MISSION_CAMERA][RX]"));

    facade.tick += 2'501U;
    runtime.Tick();
    CHECK(facade.replicatedMissionCameraSpectatorActive);
    CHECK(!facade.replicatedMissionCameraState.has_value());

    facade.missionResumePreparation = {true, true};
    pushCinematic(
        MissionCinematicPhase::PrepareResume,
        2U,
        static_cast<std::uint16_t>(
            MissionCinematicStateFlag::AnchorValid));
    facade.tick += 1U;
    runtime.Tick();
    const auto resumeReady = LastSentMissionCinematicAction(
        transport,
        MissionCinematicActionKind::ResumeReady);
    CHECK(resumeReady.has_value());
    CHECK(
        (resumeReady->flags & static_cast<std::uint16_t>(
             MissionCinematicActionFlag::FallbackUsed)) != 0U);

    facade.tick += 1U;
    pushMission(
        MissionPhase::Idle,
        3U,
        static_cast<std::uint8_t>(
            MissionStateFlag::AnchorValid));
    pushCinematic(
        MissionCinematicPhase::Completed,
        3U,
        static_cast<std::uint16_t>(
            MissionCinematicStateFlag::AnchorValid));
    runtime.Tick();
    CHECK(!facade.missionSpectatorActive);
    CHECK(!facade.replicatedMissionCameraSpectatorActive);
    CHECK(!facade.replicatedMissionCameraState.has_value());
    CHECK(!facade.missionCompanionPresentation.active);
}

void RuntimeCinematicChainsLoadingAndWaitsForResumeReady() {
    TestFacade facade;
    facade.sampledMissionCamera = MissionCameraSample{
        {10.0F, 20.0F, 30.0F},
        {-5.0F, 0.0F, 90.0F},
        55.0F,
        static_cast<std::uint32_t>(
            MissionCameraStateFlag::SourceRenderingScriptCamera)};
    TestTransport transport;
    transport.acknowledgementPayload = {
        static_cast<std::uint8_t>(PlayerSlot::Host)};
    transport.remoteSlot = PlayerSlot::Guest;
    BridgeRuntime runtime{facade, transport};
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;
    CHECK(runtime.Start(supported, error));
    runtime.Tick();

    facade.sample.missionActive = true;
    facade.sample.cutsceneActive = true;
    facade.tick += 50U;
    runtime.Tick();
    const auto first = LastSentMissionCinematicState(transport);
    CHECK(first.has_value());
    CHECK(first->phase == MissionCinematicPhase::Loading);
    const auto generation = first->cinematicGeneration;

    Frame presentationReady;
    presentationReady.header.type =
        MessageType::MissionCinematicAction;
    presentationReady.header.sequence = ++transport.inboundSequence;
    presentationReady.header.tick = facade.tick;
    presentationReady.payload = EncodeMissionCinematicAction(
        MissionCinematicActionPayload{
            runtime.LocalEntityId(),
            first->missionEpoch,
            generation,
            1U,
            MissionCinematicActionKind::PresentationReady,
            static_cast<std::uint8_t>(PlayerSlot::Guest),
            0U});
    transport.inbound.push_back(std::move(presentationReady));
    facade.tick += 1U;
    runtime.Tick();
    auto playing = LastSentMissionCinematicState(transport);
    CHECK(playing.has_value());
    CHECK(playing->phase == MissionCinematicPhase::Playing);

    facade.sampleAvailable = false;
    facade.tick += 1U;
    runtime.Tick();
    auto state = LastSentMissionCinematicState(transport);
    CHECK(state.has_value());
    CHECK(state->phase == MissionCinematicPhase::Playing);
    facade.tick += 350U;
    runtime.Tick();
    state = LastSentMissionCinematicState(transport);
    CHECK(state.has_value());
    CHECK(state->phase == MissionCinematicPhase::Loading);
    CHECK(state->cinematicGeneration == generation);

    facade.sampleAvailable = true;
    facade.sample.cutsceneActive = true;
    facade.tick += 25U;
    runtime.Tick();
    state = LastSentMissionCinematicState(transport);
    CHECK(state.has_value());
    CHECK(state->phase == MissionCinematicPhase::Playing);
    CHECK(state->cinematicGeneration == generation);

    Frame freshGuest;
    freshGuest.header.type = MessageType::PlayerState;
    freshGuest.header.sequence = ++transport.inboundSequence;
    freshGuest.header.tick = facade.tick;
    freshGuest.payload = EncodePlayerState(
        PlayerStatePayload{
            transport.remoteEntityId,
            PlayerSlot::Guest,
            PlayerLifecycle::Alive,
            transport.remotePosition,
            {},
            transport.remoteHeading,
            1.0F});
    transport.inbound.push_back(std::move(freshGuest));
    facade.sample.cutsceneActive = false;
    facade.tick += 25U;
    runtime.Tick();
    CHECK(facade.remoteMissionParticipantHidden);
    facade.tick += 749U;
    runtime.Tick();
    state = LastSentMissionCinematicState(transport);
    CHECK(state.has_value());
    CHECK(state->phase == MissionCinematicPhase::Playing);
    facade.tick += 1U;
    runtime.Tick();
    state = LastSentMissionCinematicState(transport);
    CHECK(state.has_value());
    CHECK(state->phase == MissionCinematicPhase::PrepareResume);
    CHECK(state->cinematicGeneration == generation);
    CHECK(facade.remoteMissionParticipantHidden);

    // Vanilla briefly reports its rendered camera as active again while the
    // post-scene task graph is being restored. Once PrepareResume begins this
    // rebound must never reopen Playing or the guest camera/world will flicker.
    facade.sample.cutsceneActive = true;
    facade.tick += 16U;
    runtime.Tick();
    state = LastSentMissionCinematicState(transport);
    CHECK(state.has_value());
    CHECK(state->phase == MissionCinematicPhase::PrepareResume);
    CHECK(state->cinematicGeneration == generation);
    facade.sample.cutsceneActive = false;

    facade.tick += 30'000U;
    Frame stillConnectedGuest;
    stillConnectedGuest.header.type = MessageType::PlayerState;
    stillConnectedGuest.header.sequence = ++transport.inboundSequence;
    stillConnectedGuest.header.tick = facade.tick;
    stillConnectedGuest.payload = EncodePlayerState(
        PlayerStatePayload{
            transport.remoteEntityId,
            PlayerSlot::Guest,
            PlayerLifecycle::Alive,
            transport.remotePosition,
            {},
            transport.remoteHeading,
            1.0F});
    transport.inbound.push_back(std::move(stillConnectedGuest));
    runtime.Tick();
    state = LastSentMissionCinematicState(transport);
    CHECK(state.has_value());
    CHECK(state->phase == MissionCinematicPhase::PrepareResume);
    CHECK(facade.remoteMissionParticipantHidden);
    CHECK(facade.missionResumeBarrierActive);
    CHECK(HasLog(facade, "host remains safely held"));

    Frame ready;
    ready.header.type = MessageType::MissionCinematicAction;
    ready.header.sequence = ++transport.inboundSequence;
    ready.header.tick = facade.tick;
    ready.payload = EncodeMissionCinematicAction(
        MissionCinematicActionPayload{
            runtime.LocalEntityId(),
            state->missionEpoch,
            state->cinematicGeneration,
            2U,
            MissionCinematicActionKind::ResumeReady,
            static_cast<std::uint8_t>(PlayerSlot::Guest),
            0U});
    transport.inbound.push_back(std::move(ready));
    facade.tick += 1U;
    runtime.Tick();
    state = LastSentMissionCinematicState(transport);
    CHECK(state.has_value());
    CHECK(state->phase == MissionCinematicPhase::Completed);
    CHECK(!facade.remoteMissionParticipantHidden);
    CHECK(!facade.missionResumeBarrierActive);
    // The classifier keeps the guest spectating briefly after a cinematic
    // terminal handoff, avoiding a one-frame control/UI flicker.
    facade.tick += 651U;
    runtime.Tick();
    const auto mission = LastSentMissionState(transport);
    CHECK(mission.has_value());
    CHECK(mission->phase == MissionPhase::Active);
    facade.sample.cutsceneActive = true;
    facade.tick += 16U;
    runtime.Tick();
    state = LastSentMissionCinematicState(transport);
    CHECK(state.has_value());
    CHECK(state->phase == MissionCinematicPhase::Completed);
    CHECK(LastSentMissionState(transport)->phase == MissionPhase::Active);
    facade.sample.cutsceneActive = false;
}

void RuntimeGuestRejectsOldCameraAndTearsDownOnHostLoss() {
    TestFacade facade;
    TestTransport transport;
    BridgeRuntime runtime{facade, transport};
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;
    CHECK(runtime.Start(supported, error));
    runtime.Tick();

    Frame mission;
    mission.header.type = MessageType::MissionState;
    mission.header.sequence = ++transport.inboundSequence;
    mission.header.tick = facade.tick;
    mission.payload = EncodeMissionState(
        MissionStatePayload{
            transport.remoteEntityId,
            31U,
            1U,
            1U,
            MissionPhase::Cutscene,
            static_cast<std::uint8_t>(
                MissionStateFlag::MissionActive) |
                static_cast<std::uint8_t>(
                    MissionStateFlag::AnchorValid),
            transport.remotePosition,
            transport.remoteHeading});
    transport.inbound.push_back(std::move(mission));
    Frame cinematic;
    cinematic.header.type = MessageType::MissionCinematicState;
    cinematic.header.sequence = ++transport.inboundSequence;
    cinematic.header.tick = facade.tick;
    cinematic.payload = EncodeMissionCinematicState(
        MissionCinematicStatePayload{
            transport.remoteEntityId,
            31U,
            2U,
            1U,
            1U,
            MissionCinematicPhase::Playing,
            static_cast<std::uint16_t>(
                MissionCinematicStateFlag::CameraExpected),
            {},
            0.0F});
    transport.inbound.push_back(std::move(cinematic));
    const auto pushCamera = [&](const std::uint32_t generation,
                                const std::uint32_t revision,
                                const float x) {
        Frame camera;
        camera.header.type = MessageType::MissionCameraState;
        camera.header.sequence = ++transport.inboundSequence;
        camera.header.tick = facade.tick;
        camera.payload = EncodeMissionCameraState(
            MissionCameraStatePayload{
                transport.remoteEntityId,
                31U,
                generation,
                revision,
                static_cast<std::uint32_t>(
                    MissionCameraStateFlag::Active) |
                    static_cast<std::uint32_t>(
                        MissionCameraStateFlag::SourceRenderingScriptCamera),
                {x, 20.0F, 30.0F},
                {},
                55.0F});
        transport.inbound.push_back(std::move(camera));
    };
    pushCamera(1U, 99U, 999.0F);
    pushCamera(2U, 1U, 100.0F);
    runtime.Tick();
    CHECK(facade.missionSpectatorActive);
    CHECK(facade.replicatedMissionCameraState.has_value());
    CHECK(
        facade.replicatedMissionCameraState->cinematicGeneration == 2U);
    CHECK(facade.replicatedMissionCameraState->position.x == 100.0F);
    CHECK(HasLog(facade, "stale cinematic generation"));

    facade.tick += 3'001U;
    runtime.Tick();
    CHECK(!facade.missionSpectatorActive);
    CHECK(!facade.replicatedMissionCameraState.has_value());
    runtime.Tick();
    CHECK(!facade.missionSpectatorActive);
    CHECK(HasLog(facade, "fallback teardown"));
}

void RuntimeCinematicSkipUsesStandardInputWithoutEndingMission() {
    TestFacade hostFacade;
    TestTransport hostTransport;
    hostTransport.acknowledgementPayload = {
        static_cast<std::uint8_t>(PlayerSlot::Host)};
    hostTransport.remoteSlot = PlayerSlot::Guest;
    BridgeRuntime hostRuntime{hostFacade, hostTransport};
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;
    CHECK(hostRuntime.Start(supported, error));
    hostRuntime.Tick();
    hostFacade.sample.missionActive = true;
    hostFacade.sample.cutsceneActive = true;
    hostFacade.tick += 25U;
    hostRuntime.Tick();
    hostFacade.cutsceneSkipPressed = true;
    hostFacade.tick += 1U;
    hostRuntime.Tick();
    auto cinematic = LastSentMissionCinematicState(hostTransport);
    CHECK(cinematic.has_value());
    CHECK(!hostFacade.cutsceneSkipInputActive);
    CHECK(
        (cinematic->flags & static_cast<std::uint16_t>(
             MissionCinematicStateFlag::SkipPending)) == 0U);
    // Consent is generation-scoped: subtitles or network delay must not make
    // the first player's vote disappear before the second player presses.
    hostFacade.tick += 6'001U;
    Frame freshGuestWhileVoting;
    freshGuestWhileVoting.header.type = MessageType::PlayerState;
    freshGuestWhileVoting.header.sequence =
        ++hostTransport.inboundSequence;
    freshGuestWhileVoting.header.tick = hostFacade.tick;
    freshGuestWhileVoting.payload = EncodePlayerState(
        PlayerStatePayload{
            hostTransport.remoteEntityId,
            PlayerSlot::Guest,
            PlayerLifecycle::Alive,
            hostTransport.remotePosition,
            {},
            hostTransport.remoteHeading,
            1.0F});
    hostTransport.inbound.push_back(
        std::move(freshGuestWhileVoting));
    hostRuntime.Tick();
    CHECK(!hostFacade.cutsceneSkipInputActive);
    Frame guestSkipVote;
    guestSkipVote.header.type =
        MessageType::MissionCinematicAction;
    guestSkipVote.header.sequence = ++hostTransport.inboundSequence;
    guestSkipVote.header.tick = hostFacade.tick;
    guestSkipVote.payload = EncodeMissionCinematicAction(
        MissionCinematicActionPayload{
            hostRuntime.LocalEntityId(),
            cinematic->missionEpoch,
            cinematic->cinematicGeneration,
            1U,
            MissionCinematicActionKind::SkipRequest,
            static_cast<std::uint8_t>(PlayerSlot::Guest),
            0U});
    hostTransport.inbound.push_back(std::move(guestSkipVote));
    hostFacade.tick += 1U;
    hostRuntime.Tick();
    CHECK(hostFacade.cutsceneSkipInputActive);
    cinematic = LastSentMissionCinematicState(hostTransport);
    CHECK(cinematic.has_value());
    CHECK(
        (cinematic->flags & static_cast<std::uint16_t>(
             MissionCinematicStateFlag::SkipPending)) != 0U);
    hostFacade.tick += 2'501U;
    hostRuntime.Tick();
    CHECK(!hostFacade.cutsceneSkipInputActive);
    CHECK(LastSentMissionState(hostTransport)->phase == MissionPhase::Cutscene);

    hostFacade.menuInput.f9 = true;
    hostRuntime.Tick();
    hostFacade.menuInput = {};
    hostRuntime.Tick();
    hostFacade.menuInput.confirm = true;
    hostRuntime.Tick();
    hostFacade.menuInput = {};
    hostRuntime.Tick();
    CHECK(!hostFacade.cutsceneSkipInputActive);
    Frame secondGuestSkipVote;
    secondGuestSkipVote.header.type =
        MessageType::MissionCinematicAction;
    secondGuestSkipVote.header.sequence =
        ++hostTransport.inboundSequence;
    secondGuestSkipVote.header.tick = hostFacade.tick;
    secondGuestSkipVote.payload = EncodeMissionCinematicAction(
        MissionCinematicActionPayload{
            hostRuntime.LocalEntityId(),
            cinematic->missionEpoch,
            cinematic->cinematicGeneration,
            2U,
            MissionCinematicActionKind::SkipRequest,
            static_cast<std::uint8_t>(PlayerSlot::Guest),
            0U});
    hostTransport.inbound.push_back(
        std::move(secondGuestSkipVote));
    hostFacade.tick += 1U;
    hostRuntime.Tick();
    CHECK(hostFacade.cutsceneSkipInputActive);
    CHECK(HasLog(hostFacade, "[MISSION_SKIP][CONSENSUS]"));

    TestFacade guestFacade;
    TestTransport guestTransport;
    BridgeRuntime guestRuntime{guestFacade, guestTransport};
    CHECK(guestRuntime.Start(supported, error));
    guestRuntime.Tick();
    Frame mission;
    mission.header.type = MessageType::MissionState;
    mission.header.sequence = ++guestTransport.inboundSequence;
    mission.payload = EncodeMissionState(
        MissionStatePayload{
            guestTransport.remoteEntityId,
            41U,
            1U,
            1U,
            MissionPhase::Cutscene,
            static_cast<std::uint8_t>(
                MissionStateFlag::MissionActive) |
                static_cast<std::uint8_t>(
                    MissionStateFlag::AnchorValid),
            guestTransport.remotePosition,
            guestTransport.remoteHeading});
    guestTransport.inbound.push_back(std::move(mission));
    Frame state;
    state.header.type = MessageType::MissionCinematicState;
    state.header.sequence = ++guestTransport.inboundSequence;
    state.payload = EncodeMissionCinematicState(
        MissionCinematicStatePayload{
            guestTransport.remoteEntityId,
            41U,
            3U,
            1U,
            1U,
            MissionCinematicPhase::Playing,
            static_cast<std::uint16_t>(
                MissionCinematicStateFlag::CameraExpected),
            {},
            0.0F});
    guestTransport.inbound.push_back(std::move(state));
    guestRuntime.Tick();
    guestTransport.sent.clear();
    guestFacade.cutsceneSkipPressed = true;
    guestFacade.tick += 1U;
    guestRuntime.Tick();
    const auto request = LastSentMissionCinematicAction(
        guestTransport,
        MissionCinematicActionKind::SkipRequest);
    CHECK(request.has_value());
    CHECK(request->cinematicGeneration == 3U);
    CHECK(request->senderSlot == static_cast<std::uint8_t>(PlayerSlot::Guest));

    Frame committedSkip;
    committedSkip.header.type = MessageType::MissionCinematicState;
    committedSkip.header.sequence = ++guestTransport.inboundSequence;
    committedSkip.header.tick = guestFacade.tick;
    committedSkip.payload = EncodeMissionCinematicState(
        MissionCinematicStatePayload{
            guestTransport.remoteEntityId,
            41U,
            3U,
            2U,
            1U,
            MissionCinematicPhase::Playing,
            static_cast<std::uint16_t>(
                MissionCinematicStateFlag::CameraExpected) |
                static_cast<std::uint16_t>(
                    MissionCinematicStateFlag::SkipPending),
            {},
            0.0F});
    guestTransport.inbound.push_back(std::move(committedSkip));
    guestFacade.cutsceneSkipPressed = false;
    guestFacade.tick += 1U;
    guestRuntime.Tick();
    CHECK(guestFacade.cutsceneSkipInputActive);

    Frame releasedSkip;
    releasedSkip.header.type = MessageType::MissionCinematicState;
    releasedSkip.header.sequence = ++guestTransport.inboundSequence;
    releasedSkip.header.tick = guestFacade.tick;
    releasedSkip.payload = EncodeMissionCinematicState(
        MissionCinematicStatePayload{
            guestTransport.remoteEntityId,
            41U,
            3U,
            3U,
            1U,
            MissionCinematicPhase::Playing,
            static_cast<std::uint16_t>(
                MissionCinematicStateFlag::CameraExpected),
            {},
            0.0F});
    guestTransport.inbound.push_back(std::move(releasedSkip));
    guestFacade.tick += 1U;
    guestRuntime.Tick();
    // Blind global skip is released with the authoritative consensus. V31.4
    // uses a positive captured/scanned authored-scene quarantine instead;
    // holding skip for the whole mission advanced the guest's private Story
    // VM toward Gang Abandoned/Mission Failed.
    CHECK(!guestFacade.cutsceneSkipInputActive);

    guestTransport.connectSucceeds = false;
    guestTransport.connected = false;
    guestFacade.tick += 1U;
    guestRuntime.Tick();
    guestTransport.connectSucceeds = true;
    guestFacade.tick += 1'001U;
    guestRuntime.Tick();

    Frame guestReconnectResync;
    guestReconnectResync.header.type = MessageType::ResyncRequest;
    guestReconnectResync.header.sequence =
        ++guestTransport.inboundSequence;
    guestReconnectResync.header.tick = guestFacade.tick;
    guestTransport.inbound.push_back(std::move(guestReconnectResync));
    Frame hostStateAfterGuestResync;
    hostStateAfterGuestResync.header.type = MessageType::PlayerState;
    hostStateAfterGuestResync.header.sequence =
        ++guestTransport.inboundSequence;
    hostStateAfterGuestResync.header.tick = guestFacade.tick;
    hostStateAfterGuestResync.payload = EncodePlayerState(
        PlayerStatePayload{
            guestTransport.remoteEntityId,
            guestTransport.remoteSlot,
            PlayerLifecycle::Alive,
            guestTransport.remotePosition,
            {1.0F, 0.0F, 0.0F},
            guestTransport.remoteHeading,
            1.0F,
            0U,
            {},
            0U,
            0.0F,
            0.0F,
            0.0F,
            0.0F,
            guestTransport.remoteLocomotionEpoch});
    guestTransport.inbound.push_back(
        std::move(hostStateAfterGuestResync));

    Frame replayedMission;
    replayedMission.header.type = MessageType::MissionState;
    replayedMission.header.sequence = ++guestTransport.inboundSequence;
    replayedMission.payload = EncodeMissionState(
        MissionStatePayload{
            guestTransport.remoteEntityId,
            41U,
            1U,
            1U,
            MissionPhase::Cutscene,
            static_cast<std::uint8_t>(
                MissionStateFlag::MissionActive) |
                static_cast<std::uint8_t>(
                    MissionStateFlag::AnchorValid),
            guestTransport.remotePosition,
            guestTransport.remoteHeading});
    guestTransport.inbound.push_back(std::move(replayedMission));
    Frame replayedCinematic;
    replayedCinematic.header.type = MessageType::MissionCinematicState;
    replayedCinematic.header.sequence = ++guestTransport.inboundSequence;
    replayedCinematic.payload = EncodeMissionCinematicState(
        MissionCinematicStatePayload{
            guestTransport.remoteEntityId,
            41U,
            3U,
            4U,
            1U,
            MissionCinematicPhase::Playing,
            static_cast<std::uint16_t>(
                MissionCinematicStateFlag::CameraExpected),
            {},
            0.0F});
    guestTransport.inbound.push_back(std::move(replayedCinematic));
    guestFacade.tick += 1U;
    guestRuntime.Tick();
    guestTransport.sent.clear();
    guestFacade.cutsceneSkipPressed = true;
    guestFacade.tick += 1U;
    guestRuntime.Tick();
    const auto requestAfterReconnect = LastSentMissionCinematicAction(
        guestTransport,
        MissionCinematicActionKind::SkipRequest);
    CHECK(requestAfterReconnect.has_value());
    CHECK(requestAfterReconnect->cinematicGeneration == 3U);
    CHECK(requestAfterReconnect->actionId > request->actionId);
}

void RuntimeStructuredDiagnosticsAreRateLimitedAndCorrelated() {
    TestFacade facade;
    facade.runtimeDivergenceDiagnostics.player =
        PlayerDivergenceDiagnostic{
            true,
            3.25F,
            48.0F,
            3U,
            0U,
            true,
            77U,
            900U};
    facade.runtimeDivergenceDiagnostics.entities =
        EntityDivergenceDiagnostic{
            4U,
            2U,
            3U,
            1U,
            0U,
            0U,
            1U,
            1U,
            2U,
            1U,
            NetEntityId::Compose(7U, 88U),
            5.5F,
            3.75F,
            NetEntityId::Compose(7U, 99U),
            2'500U};
    TestTransport transport;
    BridgeRuntime runtime{facade, transport};
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;
    CHECK(runtime.Start(supported, error));
    runtime.Tick();

    CHECK(HasLog(facade, "[SESSION_HEALTH]"));
    CHECK(HasLog(facade, "[MISSION_TIMELINE]"));
    CHECK(HasLog(facade, "[PLAYER_DIVERGENCE] available=1"));
    CHECK(HasLog(
        facade,
        "[PLAYER_DIVERGENCE][EVENT] state=first-divergence"));
    CHECK(HasLog(facade, "[ENTITY_DIVERGENCE] desired=4"));
    CHECK(HasLog(facade, "missing-script-owned=1"));
    CHECK(HasLog(facade, "position-error-p95-m=3.750000"));
    CHECK(HasLog(
        facade,
        "[ENTITY_DIVERGENCE][EVENT] state=first-divergence"));
    const auto healthCount = CountLogs(facade, "[SESSION_HEALTH]");
    facade.tick += 500U;
    runtime.Tick();
    CHECK(CountLogs(facade, "[SESSION_HEALTH]") == healthCount);

    facade.runtimeDivergenceDiagnostics = {};
    facade.tick += 501U;
    runtime.Tick();
    CHECK(CountLogs(facade, "[SESSION_HEALTH]") == healthCount + 1U);
    CHECK(HasLog(
        facade,
        "[PLAYER_DIVERGENCE][EVENT] state=recovered"));
    CHECK(HasLog(
        facade,
        "[ENTITY_DIVERGENCE][EVENT] state=recovered"));

    Frame mission;
    mission.header.type = MessageType::MissionState;
    mission.header.sequence = ++transport.inboundSequence;
    mission.header.tick = facade.tick;
    mission.payload = EncodeMissionState(
        MissionStatePayload{
            transport.remoteEntityId,
            9U,
            3U,
            1U,
            MissionPhase::Active,
            static_cast<std::uint8_t>(
                MissionStateFlag::MissionActive) |
                static_cast<std::uint8_t>(
                    MissionStateFlag::AnchorValid),
            transport.remotePosition,
            transport.remoteHeading});
    transport.inbound.push_back(std::move(mission));
    runtime.Tick();
    CHECK(HasLog(facade, "phase=active"));

    const auto healthBeforeMarker =
        CountLogs(facade, "[SESSION_HEALTH]");
    InvokeSaveProblemMarkerMenu(runtime, facade);
    CHECK(HasLog(facade, "[USER_MARKER] id=1"));
    CHECK(CountLogs(facade, "[USER_MARKER]") == 1U);
    const auto diagnosticMarker = LastSentCommand(
        transport,
        CommandOpcode::DiagnosticMarker);
    CHECK(diagnosticMarker.has_value());
    CHECK(diagnosticMarker->target.IsValid());
    CHECK(HasLog(facade, "[PROBLEM_SNAPSHOT] correlation="));
    CHECK(!facade.notifications.empty());
    CHECK(
        facade.notifications.back().find("MARKER ERROR #1 SAVED") !=
        std::string::npos);
    CHECK(
        CountLogs(facade, "[SESSION_HEALTH]") ==
        healthBeforeMarker + 1U);

    facade.sample.peerCombatTarget = true;
    facade.sample.meleeCombat = true;
    facade.sample.meleeAttackPressed = true;
    runtime.Tick();
    facade.sample.meleeCombat = false;
    facade.sample.meleeAttackPressed = false;
    runtime.Tick();
    CHECK(HasLog(facade, "[COMBAT_LIFECYCLE]"));
    CHECK(HasLog(facade, "correlation=action-"));

    facade.sample.peerLassoIntent = true;
    facade.sample.peerLassoActive = true;
    runtime.Tick();
    facade.sample.peerLassoIntent = false;
    facade.sample.peerLassoActive = false;
    runtime.Tick();
    CHECK(HasLog(facade, "[LASSO_LIFECYCLE]"));

    facade.sample.mount = LocalMountSample{
        123,
        {},
        0x11223344U,
        0U,
        {1.0F, 2.0F, 3.0F},
        {},
        90.0F,
        1.0F,
        true,
        false,
        false};
    runtime.Tick();
    CHECK(HasLog(facade, "[MOUNT_LIFECYCLE]"));

    for (const auto& line : facade.logs) {
        const bool structured =
            line.find("[SESSION_HEALTH]") != std::string::npos ||
            line.find("[MISSION_TIMELINE]") != std::string::npos ||
            line.find("[PLAYER_DIVERGENCE]") != std::string::npos ||
            line.find("[ENTITY_DIVERGENCE]") != std::string::npos ||
            line.find("[COMBAT_LIFECYCLE]") != std::string::npos ||
            line.find("[LASSO_LIFECYCLE]") != std::string::npos ||
            line.find("[MOUNT_LIFECYCLE]") != std::string::npos;
        if (!structured) {
            continue;
        }
        CHECK(line.find("token=") == std::string::npos);
        CHECK(line.find("ip=") == std::string::npos);
        CHECK(line.find("pointer=") == std::string::npos);
    }
}

void RuntimeSessionHealthReportsAndResetsHitchWindow() {
    TestFacade facade;
    TestTransport transport;
    BridgeRuntime runtime{facade, transport};
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;
    CHECK(runtime.Start(supported, error));
    runtime.Tick();
    CHECK(HasLog(facade, "tick-count=1"));
    CHECK(HasLog(facade, "max-tick-ms=0"));
    CHECK(HasLog(facade, "hitch-count=0"));

    facade.tick += 20U;
    runtime.Tick();
    facade.tick += 120U;
    runtime.Tick();
    facade.tick += 860U;
    runtime.Tick();

    const auto hitchWindow = std::find_if(
        facade.logs.rbegin(),
        facade.logs.rend(),
        [](const std::string& line) {
            return line.find("[SESSION_HEALTH]") !=
                   std::string::npos;
        });
    CHECK(hitchWindow != facade.logs.rend());
    CHECK(
        hitchWindow->find("tick-count=3") !=
        std::string::npos);
    CHECK(
        hitchWindow->find("average-tick-ms=333.333333") !=
        std::string::npos);
    CHECK(
        hitchWindow->find("max-tick-ms=860") !=
        std::string::npos);
    CHECK(
        hitchWindow->find("hitch-count=2") !=
        std::string::npos);

    runtime.Stop("hitch-window-test");
    facade.logs.clear();
    CHECK(runtime.Start(supported, error));
    runtime.Tick();
    const auto restartedWindow = std::find_if(
        facade.logs.rbegin(),
        facade.logs.rend(),
        [](const std::string& line) {
            return line.find("[SESSION_HEALTH]") !=
                   std::string::npos;
        });
    CHECK(restartedWindow != facade.logs.rend());
    CHECK(
        restartedWindow->find("tick-count=1") !=
        std::string::npos);
    CHECK(
        restartedWindow->find("average-tick-ms=0.000000") !=
        std::string::npos);
    CHECK(
        restartedWindow->find("max-tick-ms=0") !=
        std::string::npos);
    CHECK(
        restartedWindow->find("hitch-count=0") !=
        std::string::npos);
}

void RuntimeHostIsolatesRemoteParticipantDuringScenes() {
    TestFacade facade;
    TestTransport transport;
    transport.acknowledgementPayload = {
        static_cast<std::uint8_t>(PlayerSlot::Host)};
    transport.remoteSlot = PlayerSlot::Guest;
    transport.remoteEntityId =
        NetEntityId::Compose(0xA0B0C0D0U, 2U);
    BridgeRuntime runtime{facade, transport};
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;
    CHECK(runtime.Start(supported, error));
    runtime.Tick();
    CHECK(runtime.LocalSlot() == PlayerSlot::Host);

    facade.sample.missionActive = true;
    facade.tick += 25U;
    runtime.Tick();
    CHECK(!facade.remoteMissionParticipantHidden);

    facade.sample.cutsceneActive = true;
    facade.tick += 25U;
    runtime.Tick();
    CHECK(facade.remoteMissionParticipantHidden);
    CHECK(
        HasLog(
            facade,
            "host scene isolated the remote guest proxy"));
    const auto transformCountWhileEntering =
        facade.remoteTransforms.size();
    facade.tick += 25U;
    runtime.Tick();
    CHECK(
        facade.remoteTransforms.size() ==
        transformCountWhileEntering);

    facade.sample.cutsceneActive = false;
    facade.tick += 25U;
    runtime.Tick();
    CHECK(facade.remoteMissionParticipantHidden);

    facade.tick += 750U;
    runtime.Tick();
    const auto prepare = LastSentMissionCinematicState(transport);
    CHECK(prepare.has_value());
    CHECK(prepare->phase == MissionCinematicPhase::PrepareResume);
    CHECK(facade.remoteMissionParticipantHidden);

    Frame ready;
    ready.header.type = MessageType::MissionCinematicAction;
    ready.header.sequence = ++transport.inboundSequence;
    ready.header.tick = facade.tick;
    ready.payload = EncodeMissionCinematicAction(
        MissionCinematicActionPayload{
            runtime.LocalEntityId(),
            prepare->missionEpoch,
            prepare->cinematicGeneration,
            1U,
            MissionCinematicActionKind::ResumeReady,
            static_cast<std::uint8_t>(PlayerSlot::Guest),
            0U});
    transport.inbound.push_back(std::move(ready));
    facade.tick += 1U;
    runtime.Tick();
    CHECK(!facade.remoteMissionParticipantHidden);
    const auto completed = LastSentMissionCinematicState(transport);
    CHECK(completed.has_value());
    CHECK(completed->phase == MissionCinematicPhase::Completed);
}

void RuntimeGuestQuarantinesLocalMissionAndDefersAnchor() {
    TestFacade facade;
    TestTransport transport;
    BridgeRuntime runtime{facade, transport};
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;
    CHECK(runtime.Start(supported, error));
    runtime.Tick();
    CHECK(runtime.LocalSlot() == PlayerSlot::Guest);

    constexpr auto kMissionAndAnchor =
        static_cast<std::uint8_t>(
            MissionStateFlag::MissionActive) |
        static_cast<std::uint8_t>(
            MissionStateFlag::AnchorValid);
    Frame mission;
    mission.header.type = MessageType::MissionState;
    mission.header.sequence = ++transport.inboundSequence;
    mission.header.tick = facade.tick;
    mission.payload = EncodeMissionState(
        MissionStatePayload{
            transport.remoteEntityId,
            10U,
            1U,
            1U,
            MissionPhase::Active,
            kMissionAndAnchor,
            transport.remotePosition,
            transport.remoteHeading});
    transport.inbound.push_back(std::move(mission));
    facade.sample.missionActive = true;
    facade.distance = 900.0F;
    facade.missionIsolationStatus =
        GuestMissionIsolationStatus{true, true, false};
    facade.tick += 50U;
    runtime.Tick();
    // A contaminated guest must stay in the reversible spectator state even
    // while the host mission is Active. Releasing it into the guest's own
    // Story VM caused the V23 post-cutscene crash and Mission Failed screen.
    CHECK(facade.missionSpectatorActive);
    CHECK(facade.cutsceneSkipInputActive);
    CHECK(HasLog(facade, "[MISSION_SKIP][QUARANTINE]"));
    CHECK(
        HasLog(
            facade,
            "guest-local Story mission transition detected"));
    CHECK(
        !LastSentCommand(
             transport,
             CommandOpcode::TeleportGuest)
             .has_value());

    const auto localTeleportCount = [&]() {
        return static_cast<std::size_t>(std::count_if(
            facade.networkCommands.begin(),
            facade.networkCommands.end(),
            [](const CommandPayload& command) {
                return command.opcode ==
                       CommandOpcode::TeleportGuest;
            }));
    };
    const auto beforeDeferred = localTeleportCount();
    Frame teleport;
    teleport.header.type = MessageType::Command;
    teleport.header.sequence = ++transport.inboundSequence;
    teleport.header.tick = facade.tick;
    teleport.payload = EncodeCommand(
        CommandPayload{
            CommandOpcode::TeleportGuest,
            0U,
            runtime.LocalEntityId(),
            {100.0F, 200.0F, 30.0F},
            90.0F,
            0.0F});
    transport.inbound.push_back(std::move(teleport));
    facade.tick += 10U;
    runtime.Tick();
    CHECK(localTeleportCount() == beforeDeferred);

    facade.tick += 2'501U;
    runtime.Tick();
    // Suppression is mission-scoped now: V31.2 observed new guest-authored
    // scenes up to 98 seconds after the host cutscene, so the old bounded
    // 2500 ms window was not an authority boundary.
    CHECK(facade.cutsceneSkipInputActive);

    facade.sample.missionActive = false;
    facade.missionIsolationStatus = {};
    Frame missionEnded;
    missionEnded.header.type = MessageType::MissionState;
    missionEnded.header.sequence = ++transport.inboundSequence;
    missionEnded.header.tick = facade.tick;
    missionEnded.payload = EncodeMissionState(
        MissionStatePayload{
            transport.remoteEntityId,
            10U,
            2U,
            1U,
            MissionPhase::Idle,
            0U,
            {},
            0.0F});
    transport.inbound.push_back(std::move(missionEnded));
    facade.tick += 10U;
    runtime.Tick();
    CHECK(!facade.missionSpectatorActive);
    CHECK(!facade.cutsceneSkipInputActive);
    CHECK(localTeleportCount() == beforeDeferred + 1U);

    facade.sample.missionActive = true;
    facade.missionIsolationStatus =
        GuestMissionIsolationStatus{true, true, false};
    facade.tick += 10U;
    runtime.Tick();
    CHECK(facade.cutsceneSkipInputActive);
}

void RuntimeGuestMissionLeaseSurvivesEverySessionPhase() {
    TestFacade facade;
    TestTransport transport;
    BridgeRuntime runtime{facade, transport};
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;
    CHECK(runtime.Start(supported, error));
    runtime.Tick();
    CHECK(runtime.LocalSlot() == PlayerSlot::Guest);
    CHECK(facade.missionAuthorityActive);  // Idle.
    CHECK(!facade.missionGateAsserted);

    constexpr auto kMissionAndAnchor =
        static_cast<std::uint8_t>(
            MissionStateFlag::MissionActive) |
        static_cast<std::uint8_t>(
            MissionStateFlag::AnchorValid);
    std::uint32_t revision{};
    const auto pushPhase = [&](const MissionPhase phase) {
        Frame frame;
        frame.header.type = MessageType::MissionState;
        frame.header.sequence = ++transport.inboundSequence;
        frame.header.tick = facade.tick;
        frame.payload = EncodeMissionState(
            MissionStatePayload{
                transport.remoteEntityId,
                22U,
                ++revision,
                1U,
                phase,
                kMissionAndAnchor,
                transport.remotePosition,
                transport.remoteHeading});
        transport.inbound.push_back(std::move(frame));
        facade.tick += 25U;
        runtime.Tick();
        CHECK(facade.missionAuthorityActive);
    };

    pushPhase(MissionPhase::Active);
    CHECK(facade.hostMissionAuthorityActive);
    CHECK(!facade.hostMissionPresentationActive);
    // During an authoritative host mission the guest process must report an
    // occupied Story slot, otherwise the same local mission can start later
    // and produce its private Gang Abandoned failure. Free roam above remains
    // untouched.
    CHECK(facade.missionGateAsserted);
    pushPhase(MissionPhase::Cutscene);
    CHECK(facade.hostMissionPresentationActive);
    CHECK(facade.missionGateAsserted);
    pushPhase(MissionPhase::SoloOverride);
    CHECK(facade.hostMissionPresentationActive);

    Frame goodbye;
    goodbye.header.type = MessageType::Goodbye;
    goodbye.header.sequence = ++transport.inboundSequence;
    goodbye.header.tick = facade.tick;
    transport.inbound.push_back(std::move(goodbye));
    facade.tick += 25U;
    runtime.Tick();
    CHECK(!transport.connected);
    CHECK(facade.missionAuthorityActive);
    CHECK(
        std::find(
            facade.missionAuthorityStates.begin(),
            facade.missionAuthorityStates.end(),
            false) == facade.missionAuthorityStates.end());

    facade.tick += 1'100U;
    runtime.Tick();
    CHECK(transport.connected);
    CHECK(facade.missionAuthorityActive);
    CHECK(
        std::find(
            facade.missionAuthorityStates.begin(),
            facade.missionAuthorityStates.end(),
            false) == facade.missionAuthorityStates.end());

    const std::string stoppedMessage{"stopped"};
    Frame stopped;
    stopped.header.type = MessageType::SessionMenuStatus;
    stopped.payload = {
        static_cast<std::uint8_t>(
            SessionMenuStatusKind::Waiting),
        static_cast<std::uint8_t>(stoppedMessage.size()),
        0U,
        0U,
        0U};
    stopped.payload.insert(
        stopped.payload.end(),
        stoppedMessage.begin(),
        stoppedMessage.end());
    transport.inbound.push_back(std::move(stopped));
    facade.tick += 25U;
    runtime.Tick();
    CHECK(!facade.missionAuthorityActive);
    CHECK(!facade.missionAuthorityStates.empty());
    CHECK(!facade.missionAuthorityStates.back());
}

void RuntimeGuestAcceptsMissionStateAndGatesWorldGraph() {
    TestFacade facade;
    TestTransport transport;
    BridgeRuntime runtime{facade, transport};
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;
    CHECK(runtime.Start(supported, error));
    runtime.Tick();
    CHECK(runtime.LocalSlot() == PlayerSlot::Guest);

    constexpr auto kMissionAndAnchor =
        static_cast<std::uint8_t>(
            MissionStateFlag::MissionActive) |
        static_cast<std::uint8_t>(
            MissionStateFlag::AnchorValid);
    const auto pushMission = [&](const MissionPhase phase,
                                 const std::uint32_t revision,
                                 const std::uint32_t checkpointGeneration,
                                 const std::uint8_t flags) {
        Frame frame;
        frame.header.type = MessageType::MissionState;
        frame.header.sequence = ++transport.inboundSequence;
        frame.header.tick = facade.tick;
        frame.payload = EncodeMissionState(
            MissionStatePayload{
                transport.remoteEntityId,
                7U,
                revision,
                checkpointGeneration,
                phase,
                flags,
                transport.remotePosition,
                transport.remoteHeading});
        transport.inbound.push_back(std::move(frame));
    };
    const WorldEntityStatePayload actor{
        NetEntityId::Compose(0x12345678U, 1'001U),
        0x10203040U,
        WorldEntityKind::Ped,
        static_cast<std::uint8_t>(
            WorldEntityStateFlag::Human),
        WorldCombatTargetSlot::None,
        {11.0F, 20.0F, 30.0F},
        {},
        180.0F,
        1.0F,
        0U};
    const auto pushEntity = [&](const MessageType type) {
        Frame frame;
        frame.header.type = type;
        frame.header.sequence = ++transport.inboundSequence;
        frame.header.tick = facade.tick;
        frame.payload = EncodeWorldEntityState(actor);
        transport.inbound.push_back(std::move(frame));
    };

    pushMission(MissionPhase::Active, 1U, 1U, kMissionAndAnchor);
    pushEntity(MessageType::EntitySpawn);
    runtime.Tick();
    CHECK(facade.missionAuthorityActive);
    CHECK(facade.hostMissionAuthorityActive);
    CHECK(!facade.missionSpectatorActive);
    CHECK(facade.worldEntitySpawns.size() == 1U);
    CHECK(HasLog(facade, "[MISSION_RX][MISSION_FSM]"));

    facade.tick += 50U;
    pushMission(MissionPhase::Cutscene, 2U, 1U, kMissionAndAnchor);
    pushEntity(MessageType::EntityUpdate);
    runtime.Tick();
    CHECK(facade.missionSpectatorActive);
    CHECK(facade.worldEntityUpdates.empty());
    // Entering a camera presentation is not a checkpoint/world-generation
    // boundary. Preserve the existing host cast for exact binding or the
    // visible proxy fallback; only the local Story population stays masked.
    CHECK(facade.worldMirrorGuestActive);
    CHECK(facade.worldEntityDespawns.empty());
    CHECK(
        HasLog(
            facade,
            "retained stable host cast"));

    facade.tick += 50U;
    pushMission(MissionPhase::Active, 3U, 1U, kMissionAndAnchor);
    pushEntity(MessageType::EntityUpdate);
    runtime.Tick();
    CHECK(!facade.missionSpectatorActive);
    CHECK(facade.worldEntitySpawns.size() == 1U);
    // The frame that releases spectator is intentionally conservative: its
    // entity update is deferred while the camera/body transition completes.
    // The following authoritative update is accepted as an upsert.
    facade.tick += 1U;
    pushEntity(MessageType::EntityUpdate);
    runtime.Tick();
    // The retained actor continues through the transition without a despawn
    // and respawn/T-pose cycle.
    CHECK(facade.worldEntitySpawns.size() == 1U);
    CHECK(facade.worldEntityUpdates.size() == 1U);
    CHECK(facade.worldMirrorGuestActive);

    facade.tick += 50U;
    pushMission(
        MissionPhase::Recovery,
        4U,
        2U,
        static_cast<std::uint8_t>(
            kMissionAndAnchor |
            static_cast<std::uint8_t>(
                MissionStateFlag::CheckpointRecovery)));
    const auto updatesBeforeCheckpointRecovery =
        facade.worldEntityUpdates.size();
    pushEntity(MessageType::EntityUpdate);
    runtime.Tick();
    CHECK(facade.missionSpectatorActive);
    CHECK(
        facade.worldEntityUpdates.size() ==
        updatesBeforeCheckpointRecovery);
    CHECK(facade.worldMirrorGuestActive);
    CHECK(HasLog(facade, "[MISSION_CHECKPOINT][MISSION_RX]"));
}

void RuntimeHostStreamsAndCleansWorldMirror() {
    TestFacade facade;
    facade.sampledWorldEntities.push_back(
        HostWorldEntitySample{
            333,
            0x10203040U,
            WorldEntityKind::Ped,
            static_cast<std::uint8_t>(
                WorldEntityStateFlag::Human),
            WorldCombatTargetSlot::None,
            {11.0F, 20.0F, 30.0F},
            {1.0F, 0.0F, 0.0F},
            45.0F,
            1.0F,
            0x55667788U});
    TestTransport transport;
    transport.acknowledgementPayload = {
        static_cast<std::uint8_t>(
            PlayerSlot::Host)};
    transport.remoteSlot = PlayerSlot::Guest;
    transport.remoteEntityId =
        NetEntityId::Compose(0xA0B0C0D0U, 2U);
    BridgeRuntime runtime{facade, transport};
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;
    CHECK(runtime.Start(supported, error));
    runtime.Tick();

    const auto spawn = std::find_if(
        transport.sent.begin(),
        transport.sent.end(),
        [](const Frame& frame) {
            return frame.header.type ==
                   MessageType::EntitySpawn;
        });
    CHECK(spawn != transport.sent.end());
    const auto spawned =
        DecodeWorldEntityState(spawn->payload);
    CHECK(spawned.has_value());
    CHECK(spawned->modelHash == 0x10203040U);

    facade.tick += 100U;
    runtime.Tick();
    CHECK(
        std::any_of(
            transport.sent.begin(),
            transport.sent.end(),
            [](const Frame& frame) {
                return frame.header.type ==
                       MessageType::EntityUpdate;
            }));

    const auto sentBeforePipeFailure = transport.sent.size();
    transport.connectSucceeds = false;
    transport.disconnectOnNextPoll = true;
    transport.injectRemoteState = false;
    facade.tick += 100U;
    runtime.Tick();
    CHECK(!transport.connected);
    CHECK(
        std::none_of(
            transport.sent.begin() +
                static_cast<std::ptrdiff_t>(sentBeforePipeFailure),
            transport.sent.end(),
            [](const Frame& frame) {
                return frame.header.type == MessageType::EntityDespawn;
            }));
    facade.tick += 100U;
    runtime.Tick();
    transport.connectSucceeds = true;
    const auto sentBeforeReconnect = transport.sent.size();
    facade.tick += 1'001U;
    runtime.Tick();
    CHECK(
        std::none_of(
            transport.sent.begin() +
                static_cast<std::ptrdiff_t>(sentBeforeReconnect),
            transport.sent.end(),
            [](const Frame& frame) {
                return frame.header.type == MessageType::EntitySpawn ||
                       frame.header.type == MessageType::EntityDespawn;
            }));
    Frame restoredGuestState;
    restoredGuestState.header.type = MessageType::PlayerState;
    restoredGuestState.header.sequence = ++transport.inboundSequence;
    restoredGuestState.header.tick = facade.tick;
    restoredGuestState.payload = EncodePlayerState(
        PlayerStatePayload{
            transport.remoteEntityId,
            transport.remoteSlot,
            PlayerLifecycle::Alive,
            transport.remotePosition,
            {1.0F, 0.0F, 0.0F},
            transport.remoteHeading,
            1.0F,
            0U,
            {},
            0U,
            0.0F,
            0.0F,
            0.0F,
            0.0F,
            transport.remoteLocomotionEpoch});
    transport.inbound.push_back(std::move(restoredGuestState));
    transport.injectRemoteState = true;
    facade.tick += 1U;
    runtime.Tick();
    const auto replayedSpawn = std::find_if(
        transport.sent.begin() +
            static_cast<std::ptrdiff_t>(sentBeforeReconnect),
        transport.sent.end(),
        [](const Frame& frame) {
            return frame.header.type == MessageType::EntitySpawn;
        });
    CHECK(replayedSpawn != transport.sent.end());
    const auto replayedEntity =
        DecodeWorldEntityState(replayedSpawn->payload);
    CHECK(replayedEntity.has_value());
    CHECK(replayedEntity->entityId == spawned->entityId);

    facade.sample.missionActive = true;
    facade.sample.cutsceneActive = true;
    const auto updatesBeforeCutscene = std::count_if(
        transport.sent.begin(),
        transport.sent.end(),
        [](const Frame& frame) {
            return frame.header.type == MessageType::EntityUpdate;
        });
    facade.tick += 100U;
    runtime.Tick();
    const auto despawn = std::find_if(
        transport.sent.begin(),
        transport.sent.end(),
        [](const Frame& frame) {
            return frame.header.type ==
                   MessageType::EntityDespawn;
        });
    CHECK(despawn == transport.sent.end());
    const auto updatesDuringCutscene = std::count_if(
        transport.sent.begin(),
        transport.sent.end(),
        [](const Frame& frame) {
            return frame.header.type == MessageType::EntityUpdate;
            });
    CHECK(updatesDuringCutscene > updatesBeforeCutscene);

    // WorldMirrorHost mutates its local graph before the IPC write. If the
    // pipe breaks on the resulting Despawn, retain a tombstone and deliver it
    // before any post-reconnect graph traffic; otherwise the sidecar/guest
    // would keep an orphan ped/horse forever.
    facade.sample.cutsceneActive = false;
    facade.sampledWorldEntities.clear();
    transport.failNextEntityDespawn = true;
    const auto sentBeforeFailedDespawn = transport.sent.size();
    facade.tick += 800U;
    runtime.Tick();
    CHECK(!transport.connected);
    CHECK(
        std::none_of(
            transport.sent.begin() +
                static_cast<std::ptrdiff_t>(sentBeforeFailedDespawn),
            transport.sent.end(),
            [](const Frame& frame) {
                return frame.header.type == MessageType::EntityDespawn;
            }));
    const auto sentBeforeTombstoneReconnect = transport.sent.size();
    facade.tick += 1'001U;
    runtime.Tick();
    const auto replayedDespawn = std::find_if(
        transport.sent.begin() +
            static_cast<std::ptrdiff_t>(sentBeforeTombstoneReconnect),
        transport.sent.end(),
        [](const Frame& frame) {
            return frame.header.type == MessageType::EntityDespawn;
        });
    CHECK(replayedDespawn != transport.sent.end());
    const auto replayedDespawnPayload =
        DecodeEntityDespawn(replayedDespawn->payload);
    CHECK(replayedDespawnPayload.has_value());
    CHECK(replayedDespawnPayload->entityId == spawned->entityId);
}

void RuntimeGuestUpsertsAndCleansWorldMirror() {
    TestFacade facade;
    TestTransport transport;
    BridgeRuntime runtime{facade, transport};
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;
    CHECK(runtime.Start(supported, error));
    runtime.Tick();
    CHECK(runtime.LocalSlot() == PlayerSlot::Guest);

    const WorldEntityStatePayload state{
        NetEntityId::Compose(0x12345678U, 1'000U),
        0x10203040U,
        WorldEntityKind::Ped,
        static_cast<std::uint8_t>(
            WorldEntityStateFlag::Human) |
            static_cast<std::uint8_t>(
                WorldEntityStateFlag::InCombat),
        WorldCombatTargetSlot::Host,
        {10.0F, 20.0F, 30.0F},
        {1.0F, 0.0F, 0.0F},
        180.0F,
        0.75F,
        0x55667788U};
    Frame update;
    update.header.type = MessageType::EntityUpdate;
    update.header.sequence = ++transport.inboundSequence;
    update.header.tick = facade.tick;
    update.payload = EncodeWorldEntityState(state);
    transport.inbound.push_back(std::move(update));
    runtime.Tick();
    CHECK(facade.worldEntitySpawns.size() == 1U);
    CHECK(
        facade.worldEntitySpawns.front().entityId ==
        state.entityId);
    CHECK(facade.worldMirrorGuestActive);

    Frame despawn;
    despawn.header.type = MessageType::EntityDespawn;
    despawn.header.sequence = ++transport.inboundSequence;
    despawn.header.tick = facade.tick;
    despawn.payload = EncodeEntityDespawn(
        EntityDespawnPayload{state.entityId});
    transport.inbound.push_back(std::move(despawn));
    runtime.Tick();
    CHECK(facade.worldEntityDespawns.size() == 1U);

    Frame delayedUpdate;
    delayedUpdate.header.type = MessageType::EntityUpdate;
    delayedUpdate.header.sequence =
        transport.inboundSequence - 1U;
    delayedUpdate.header.tick = facade.tick;
    delayedUpdate.payload =
        EncodeWorldEntityState(state);
    transport.inbound.push_back(std::move(delayedUpdate));
    runtime.Tick();
    CHECK(facade.worldEntitySpawns.size() == 1U);
    CHECK(facade.worldEntityUpdates.empty());

    facade.sample.missionActive = true;
    runtime.Tick();
    // Guest-local mission markers are not authoritative. The host's player
    // state decides whether World Mirror remains active.
    CHECK(facade.worldMirrorGuestActive);
}

void RuntimeHostValidatesGuestDamageIntent() {
    TestFacade facade;
    facade.sampledWorldEntities.push_back(
        HostWorldEntitySample{
            444,
            0x10203040U,
            WorldEntityKind::Ped,
            static_cast<std::uint8_t>(
                WorldEntityStateFlag::Human),
            WorldCombatTargetSlot::None,
            {11.0F, 20.0F, 30.0F},
            {},
            0.0F,
            1.0F,
            0x55667788U});
    TestTransport transport;
    transport.acknowledgementPayload = {
        static_cast<std::uint8_t>(
            PlayerSlot::Host)};
    transport.remoteSlot = PlayerSlot::Guest;
    transport.remoteEntityId =
        NetEntityId::Compose(0xA0B0C0D0U, 2U);
    BridgeRuntime runtime{facade, transport};
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;
    CHECK(runtime.Start(supported, error));
    runtime.Tick();

    const auto spawn = std::find_if(
        transport.sent.begin(),
        transport.sent.end(),
        [](const Frame& frame) {
            return frame.header.type ==
                   MessageType::EntitySpawn;
        });
    CHECK(spawn != transport.sent.end());
    const auto target =
        DecodeWorldEntityState(spawn->payload);
    CHECK(target.has_value());

    constexpr std::uint32_t weaponHash =
        0x55667788U;
    Frame firing;
    firing.header.type = MessageType::PlayerState;
    firing.header.sequence = ++transport.inboundSequence;
    firing.header.tick = facade.tick;
    firing.payload = EncodePlayerState(
        PlayerStatePayload{
            transport.remoteEntityId,
            PlayerSlot::Guest,
            PlayerLifecycle::Alive,
            transport.remotePosition,
            {},
            transport.remoteHeading,
            1.0F,
            static_cast<std::uint32_t>(
                PlayerStateFlag::Firing)});
    transport.inbound.push_back(std::move(firing));

    Frame equipment;
    equipment.header.type = MessageType::EquipmentState;
    equipment.header.sequence = ++transport.inboundSequence;
    equipment.header.tick = facade.tick;
    equipment.payload = EncodeEquipmentState(
        EquipmentStatePayload{
            transport.remoteEntityId,
            weaponHash,
            24U,
            static_cast<std::uint32_t>(
                EquipmentStateFlag::Equipped)});
    transport.inbound.push_back(std::move(equipment));

    Frame damage;
    damage.header.type = MessageType::DamageIntent;
    damage.header.sequence = ++transport.inboundSequence;
    damage.header.tick = facade.tick;
    damage.payload = EncodeDamageIntent(
        DamageIntentPayload{
            transport.remoteEntityId,
            target->entityId,
            weaponHash,
            25.0F,
            100U});
    transport.inbound.push_back(std::move(damage));
    runtime.Tick();

    CHECK(facade.appliedWorldDamage.size() == 1U);
    CHECK(facade.appliedWorldDamage.front().first == 444);
    CHECK(facade.appliedWorldDamage.front().second == 25.0F);

    const auto replacementGuest =
        NetEntityId::Compose(0xB0C0D0E0U, 2U);
    Frame replacementFiring;
    replacementFiring.header.type =
        MessageType::PlayerState;
    replacementFiring.header.sequence =
        ++transport.inboundSequence;
    replacementFiring.header.tick = facade.tick;
    replacementFiring.payload = EncodePlayerState(
        PlayerStatePayload{
            replacementGuest,
            PlayerSlot::Guest,
            PlayerLifecycle::Alive,
            transport.remotePosition,
            {},
            transport.remoteHeading,
            1.0F,
            static_cast<std::uint32_t>(
                PlayerStateFlag::Firing)});
    transport.inbound.push_back(
        std::move(replacementFiring));

    Frame replacementEquipment;
    replacementEquipment.header.type =
        MessageType::EquipmentState;
    replacementEquipment.header.sequence =
        ++transport.inboundSequence;
    replacementEquipment.header.tick = facade.tick;
    replacementEquipment.payload = EncodeEquipmentState(
        EquipmentStatePayload{
            replacementGuest,
            weaponHash,
            24U,
            static_cast<std::uint32_t>(
                EquipmentStateFlag::Equipped)});
    transport.inbound.push_back(
        std::move(replacementEquipment));

    Frame replacementDamage;
    replacementDamage.header.type =
        MessageType::DamageIntent;
    replacementDamage.header.sequence =
        ++transport.inboundSequence;
    replacementDamage.header.tick = facade.tick;
    replacementDamage.payload = EncodeDamageIntent(
        DamageIntentPayload{
            replacementGuest,
            target->entityId,
            weaponHash,
            25.0F,
            1U});
    transport.inbound.push_back(
        std::move(replacementDamage));
    runtime.Tick();
    // A replacement entity ID cannot silently take over an authenticated
    // slot; reconnect/Hello must establish the new identity first.
    CHECK(facade.appliedWorldDamage.size() == 1U);
    CHECK(
        HasLog(
            facade,
            "identity disagrees with authenticated mission state"));
}

void RuntimeScriptOwnedDamageRequiresActiveHostMission() {
    constexpr auto kScriptHostileFlags =
        static_cast<std::uint8_t>(
            WorldEntityStateFlag::Human) |
        static_cast<std::uint8_t>(
            WorldEntityStateFlag::InCombat) |
        static_cast<std::uint8_t>(
            WorldEntityStateFlag::ScriptOwned);
    TestFacade facade;
    facade.sampledWorldEntities.push_back(
        HostWorldEntitySample{
            777,
            0x10203040U,
            WorldEntityKind::Ped,
            kScriptHostileFlags,
            WorldCombatTargetSlot::Guest,
            {11.0F, 20.0F, 30.0F},
            {},
            0.0F,
            1.0F,
            0x55667788U});
    TestTransport transport;
    transport.acknowledgementPayload = {
        static_cast<std::uint8_t>(PlayerSlot::Host)};
    transport.remoteSlot = PlayerSlot::Guest;
    transport.remoteEntityId =
        NetEntityId::Compose(0xA0B0C0D0U, 2U);
    BridgeRuntime runtime{facade, transport};
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;
    CHECK(runtime.Start(supported, error));
    runtime.Tick();

    const auto spawn = std::find_if(
        transport.sent.begin(),
        transport.sent.end(),
        [](const Frame& frame) {
            return frame.header.type ==
                   MessageType::EntitySpawn;
        });
    CHECK(spawn != transport.sent.end());
    const auto target = DecodeWorldEntityState(spawn->payload);
    CHECK(target.has_value());
    CHECK(
        (target->flags &
         static_cast<std::uint8_t>(
             WorldEntityStateFlag::ScriptOwned)) != 0U);

    constexpr std::uint32_t kWeaponHash = 0x55667788U;
    Frame equipment;
    equipment.header.type = MessageType::EquipmentState;
    equipment.header.sequence = ++transport.inboundSequence;
    equipment.header.tick = facade.tick;
    equipment.payload = EncodeEquipmentState(
        EquipmentStatePayload{
            transport.remoteEntityId,
            kWeaponHash,
            24U,
            static_cast<std::uint32_t>(
                EquipmentStateFlag::Equipped)});
    transport.inbound.push_back(std::move(equipment));
    runtime.Tick();

    const auto sendDamage = [&](const std::uint32_t shotSequence) {
        Frame damage;
        damage.header.type = MessageType::DamageIntent;
        damage.header.sequence = ++transport.inboundSequence;
        damage.header.tick = facade.tick;
        damage.payload = EncodeDamageIntent(
            DamageIntentPayload{
                transport.remoteEntityId,
                target->entityId,
                kWeaponHash,
                25.0F,
                shotSequence});
        transport.inbound.push_back(std::move(damage));
    };

    sendDamage(1U);
    runtime.Tick();
    CHECK(facade.appliedWorldDamage.empty());
    CHECK(facade.appliedMissionWorldDamage.empty());
    CHECK(
        HasLog(
            facade,
            "script-owned-target-without-active-host-mission"));

    // Mission authority is sampled after inbound control processing, so
    // establish Active on one host tick before accepting the next intent.
    facade.sample.missionActive = true;
    facade.tick += 100U;
    runtime.Tick();
    const auto mission = LastSentMissionState(transport);
    CHECK(mission.has_value());
    CHECK(mission->phase == MissionPhase::Active);

    sendDamage(2U);
    runtime.Tick();
    CHECK(facade.appliedWorldDamage.empty());
    CHECK(facade.appliedMissionWorldDamage.size() == 1U);
    const auto& [handle, weaponHash, damage] =
        facade.appliedMissionWorldDamage.front();
    CHECK(handle == 777);
    CHECK(weaponHash == kWeaponHash);
    CHECK(damage == 25.0F);
    CHECK(
        HasLog(
            facade,
            "accepted path=script-owned-attributed-projectile"));
}

void RuntimeWorldMirrorUsesHostMissionAuthority() {
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;

    TestFacade hostFacade;
    hostFacade.sampledWorldEntities.push_back(
        HostWorldEntitySample{
            501,
            0x10203040U,
            WorldEntityKind::Ped,
            static_cast<std::uint8_t>(
                WorldEntityStateFlag::Human),
            WorldCombatTargetSlot::None,
            {11.0F, 20.0F, 30.0F},
            {},
            0.0F,
            1.0F,
            0U});
    TestTransport hostTransport;
    hostTransport.acknowledgementPayload = {
        static_cast<std::uint8_t>(PlayerSlot::Host)};
    hostTransport.remoteSlot = PlayerSlot::Guest;
    hostTransport.remoteEntityId =
        NetEntityId::Compose(0xA0B0C0D0U, 2U);
    BridgeRuntime hostRuntime{hostFacade, hostTransport};
    CHECK(hostRuntime.Start(supported, error));
    hostRuntime.Tick();
    CHECK(
        CountSentFrames(
            hostTransport,
            MessageType::EntitySpawn) == 1U);

    hostTransport.sent.clear();
    hostFacade.tick += 100U;
    Frame guestMissionState;
    guestMissionState.header.type = MessageType::PlayerState;
    guestMissionState.header.sequence =
        ++hostTransport.inboundSequence;
    guestMissionState.header.tick = hostFacade.tick;
    guestMissionState.payload = EncodePlayerState(
        PlayerStatePayload{
            hostTransport.remoteEntityId,
            PlayerSlot::Guest,
            PlayerLifecycle::Alive,
            hostTransport.remotePosition,
            {},
            hostTransport.remoteHeading,
            1.0F,
            static_cast<std::uint32_t>(
                PlayerStateFlag::InMission)});
    hostTransport.inbound.push_back(
        std::move(guestMissionState));
    hostRuntime.Tick();
    CHECK(
        CountSentFrames(
            hostTransport,
            MessageType::EntityUpdate) == 1U);
    CHECK(
        CountSentFrames(
            hostTransport,
            MessageType::EntityDespawn) == 0U);

    TestFacade guestFacade;
    TestTransport guestTransport;
    BridgeRuntime guestRuntime{guestFacade, guestTransport};
    CHECK(guestRuntime.Start(supported, error));
    guestRuntime.Tick();
    CHECK(guestRuntime.LocalSlot() == PlayerSlot::Guest);
    CHECK(guestFacade.worldMirrorGuestActive);

    guestFacade.tick += 50U;
    Frame hostMissionState;
    hostMissionState.header.type = MessageType::PlayerState;
    hostMissionState.header.sequence =
        ++guestTransport.inboundSequence;
    hostMissionState.header.tick = guestFacade.tick;
    hostMissionState.payload = EncodePlayerState(
        PlayerStatePayload{
            guestTransport.remoteEntityId,
            PlayerSlot::Host,
            PlayerLifecycle::Alive,
            guestTransport.remotePosition,
            {},
            guestTransport.remoteHeading,
            1.0F,
            static_cast<std::uint32_t>(
                PlayerStateFlag::InMission)});
    guestTransport.inbound.push_back(
        std::move(hostMissionState));
    guestRuntime.Tick();
    CHECK(guestFacade.worldMirrorGuestActive);
    CHECK(guestFacade.missionAuthorityActive);
    CHECK(guestFacade.hostMissionAuthorityActive);

    guestRuntime.Stop("test complete");
    CHECK(!guestFacade.missionAuthorityActive);
    CHECK(!guestFacade.hostMissionAuthorityActive);
}

void RuntimeDamageIntentToleratesReorderAndKeepsGuards() {
    TestFacade facade;
    facade.sampledWorldEntities.push_back(
        HostWorldEntitySample{
            502,
            0x10203040U,
            WorldEntityKind::Ped,
            static_cast<std::uint8_t>(
                WorldEntityStateFlag::Human),
            WorldCombatTargetSlot::None,
            {11.0F, 20.0F, 30.0F},
            {},
            0.0F,
            1.0F,
            0U});
    TestTransport transport;
    transport.acknowledgementPayload = {
        static_cast<std::uint8_t>(PlayerSlot::Host)};
    transport.remoteSlot = PlayerSlot::Guest;
    transport.remoteEntityId =
        NetEntityId::Compose(0xA0B0C0D0U, 2U);
    BridgeRuntime runtime{facade, transport};
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;
    CHECK(runtime.Start(supported, error));
    runtime.Tick();

    const auto spawn = std::find_if(
        transport.sent.begin(),
        transport.sent.end(),
        [](const Frame& frame) {
            return frame.header.type ==
                   MessageType::EntitySpawn;
        });
    CHECK(spawn != transport.sent.end());
    const auto target =
        DecodeWorldEntityState(spawn->payload);
    CHECK(target.has_value());

    constexpr std::uint32_t weaponHash =
        0x55667788U;
    facade.tick += 50U;
    Frame nonFiringState;
    nonFiringState.header.type = MessageType::PlayerState;
    nonFiringState.header.sequence =
        ++transport.inboundSequence;
    nonFiringState.header.tick = facade.tick;
    nonFiringState.payload = EncodePlayerState(
        PlayerStatePayload{
            transport.remoteEntityId,
            PlayerSlot::Guest,
            PlayerLifecycle::Alive,
            transport.remotePosition,
            {},
            transport.remoteHeading,
            1.0F,
            0U});
    transport.inbound.push_back(std::move(nonFiringState));

    Frame equipment;
    equipment.header.type = MessageType::EquipmentState;
    equipment.header.sequence = ++transport.inboundSequence;
    equipment.header.tick = facade.tick;
    equipment.payload = EncodeEquipmentState(
        EquipmentStatePayload{
            transport.remoteEntityId,
            weaponHash,
            24U,
            static_cast<std::uint32_t>(
                EquipmentStateFlag::Equipped)});
    transport.inbound.push_back(std::move(equipment));

    const auto queueDamage =
        [&transport, &facade, &target](
            const std::uint32_t sequence,
            const std::uint32_t intentWeapon) {
            Frame damage;
            damage.header.type = MessageType::DamageIntent;
            damage.header.sequence =
                ++transport.inboundSequence;
            damage.header.tick = facade.tick;
            damage.payload = EncodeDamageIntent(
                DamageIntentPayload{
                    transport.remoteEntityId,
                    target->entityId,
                    intentWeapon,
                    25.0F,
                    sequence});
            transport.inbound.push_back(std::move(damage));
        };

    queueDamage(100U, weaponHash);
    runtime.Tick();
    CHECK(facade.appliedWorldDamage.size() == 1U);
    CHECK(facade.appliedWorldDamage.front().first == 502);

    queueDamage(101U, weaponHash);
    runtime.Tick();
    CHECK(facade.appliedWorldDamage.size() == 1U);

    facade.tick += 100U;
    queueDamage(102U, 0xDEADBEEFU);
    runtime.Tick();
    CHECK(facade.appliedWorldDamage.size() == 1U);

    facade.tick += 100U;
    Frame distantState;
    distantState.header.type = MessageType::PlayerState;
    distantState.header.sequence =
        ++transport.inboundSequence;
    distantState.header.tick = facade.tick;
    distantState.payload = EncodePlayerState(
        PlayerStatePayload{
            transport.remoteEntityId,
            PlayerSlot::Guest,
            PlayerLifecycle::Alive,
            {250.0F, 20.0F, 30.0F},
            {},
            transport.remoteHeading,
            1.0F,
            0U});
    transport.inbound.push_back(std::move(distantState));
    queueDamage(103U, weaponHash);
    runtime.Tick();
    CHECK(facade.appliedWorldDamage.size() == 1U);
}

void RuntimeHostEnforcesLifecycleAuthority() {
    TestFacade facade;
    TestTransport transport;
    transport.acknowledgementPayload = {
        static_cast<std::uint8_t>(PlayerSlot::Host)};
    transport.remoteSlot = PlayerSlot::Guest;
    transport.remoteEntityId =
        NetEntityId::Compose(0xA0B0C0D0U, 2U);
    BridgeRuntime runtime{facade, transport};
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;
    CHECK(runtime.Start(supported, error));
    runtime.Tick();
    CHECK(runtime.LocalSlot() == PlayerSlot::Host);

    Frame unauthorizedHostDowned;
    unauthorizedHostDowned.header.type =
        MessageType::DownedState;
    unauthorizedHostDowned.payload = EncodeDownedState(
        DownedStatePayload{
            runtime.LocalEntityId(),
            PlayerLifecycle::Downed,
            0.0F});
    transport.inbound.push_back(
        std::move(unauthorizedHostDowned));

    Frame authorizedGuestDowned;
    authorizedGuestDowned.header.type =
        MessageType::DownedState;
    authorizedGuestDowned.payload = EncodeDownedState(
        DownedStatePayload{
            transport.remoteEntityId,
            PlayerLifecycle::Downed,
            0.0F});
    transport.inbound.push_back(
        std::move(authorizedGuestDowned));
    runtime.Tick();
    CHECK(facade.retryCount == 0U);

    Frame unauthorizedReviveComplete;
    unauthorizedReviveComplete.header.type =
        MessageType::ReviveComplete;
    unauthorizedReviveComplete.payload =
        EncodeReviveComplete(
            ReviveCompletePayload{
                runtime.LocalEntityId(),
                transport.remoteEntityId,
                0.35F});
    transport.inbound.push_back(
        std::move(unauthorizedReviveComplete));

    Frame unauthorizedSpectator;
    unauthorizedSpectator.header.type =
        MessageType::SpectatorState;
    unauthorizedSpectator.payload = EncodeDownedState(
        DownedStatePayload{
            transport.remoteEntityId,
            PlayerLifecycle::Spectator,
            0.0F});
    transport.inbound.push_back(
        std::move(unauthorizedSpectator));
    runtime.Tick();
    CHECK(facade.retryCount == 0U);

    facade.sample.downed = true;
    runtime.Tick();
    // A mutual down is recoverable through the normal revive flow.  It must
    // never silently discard the checkpoint; only an explicit host menu choice
    // may issue a retry request.
    CHECK(facade.retryCount == 0U);
}

void RuntimeResyncEquipmentRespectsRoleAuthority() {
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;

    TestFacade guestFacade;
    guestFacade.sample.weaponHash = 0x10203040U;
    guestFacade.sample.weaponAmmo = 36U;
    TestTransport guestTransport;
    BridgeRuntime guestRuntime{guestFacade, guestTransport};
    CHECK(guestRuntime.Start(supported, error));
    guestRuntime.Tick();
    guestTransport.sent.clear();

    InvokeResyncEquipmentMenu(
        guestRuntime,
        guestFacade);
    CHECK(
        CountSentFrames(
            guestTransport,
            MessageType::EquipmentState) == 1U);
    CHECK(
        !LastSentCommand(
             guestTransport,
             CommandOpcode::ResyncEquipment)
             .has_value());

    TestFacade hostFacade;
    hostFacade.sample.weaponHash = 0x55667788U;
    hostFacade.sample.weaponAmmo = 24U;
    TestTransport hostTransport;
    hostTransport.acknowledgementPayload = {
        static_cast<std::uint8_t>(PlayerSlot::Host)};
    hostTransport.remoteSlot = PlayerSlot::Guest;
    hostTransport.remoteEntityId =
        NetEntityId::Compose(0xA0B0C0D0U, 2U);
    BridgeRuntime hostRuntime{hostFacade, hostTransport};
    CHECK(hostRuntime.Start(supported, error));
    hostRuntime.Tick();
    hostTransport.sent.clear();

    InvokeResyncEquipmentMenu(
        hostRuntime,
        hostFacade);
    CHECK(
        CountSentFrames(
            hostTransport,
            MessageType::EquipmentState) == 1U);
    CHECK(
        LastSentCommand(
            hostTransport,
            CommandOpcode::ResyncEquipment)
            .has_value());
}

void RuntimeSessionOverlay() {
    TestFacade facade;
    TestTransport transport;
    transport.acknowledgeHello = false;
    transport.injectRemoteState = false;
    BridgeRuntime runtime{facade, transport};
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;
    CHECK(runtime.Start(supported, error));

    // F8 is a closed-by-default emergency panel. HOST/JOIN/password live only
    // in the launcher, so its first action is stopping the current session.
    facade.menuInput.f8 = true;
    runtime.Tick();
    facade.menuInput = {};
    runtime.Tick();
    CHECK(facade.sessionMenuOpen);
    CHECK(facade.sessionMenuActionCount == 3U);
    CHECK(
        facade.sessionMenuFirstAction ==
        SessionOverlayAction::StopSession);
    facade.menuInput.confirm = true;
    runtime.Tick();
    facade.menuInput = {};
    CHECK(transport.sent.size() == 1U);
    CHECK(facade.sessionMenuPhase == SessionOverlayPhase::Error);
    CHECK(
        facade.sessionMenuStatus.find("There is no active session") !=
        std::string::npos);
}

void RuntimeJoinRequiresFreshClipboardCode() {
    TestFacade facade;
    TestTransport transport;
    transport.acknowledgeHello = false;
    transport.injectRemoteState = false;
    BridgeRuntime runtime{facade, transport};
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;
    CHECK(runtime.Start(supported, error));
    CHECK(transport.sent.size() == 1U);
    CHECK(
        transport.sent.front().header.type ==
        MessageType::Hello);

    facade.menuInput.f8 = true;
    runtime.Tick();
    facade.menuInput = {};
    runtime.Tick();
    facade.menuInput.down = true;
    runtime.Tick();
    facade.menuInput = {};
    runtime.Tick();
    facade.menuInput.confirm = true;
    runtime.Tick();

    CHECK(transport.sent.size() == 1U);
    CHECK(
        facade.sessionMenuPhase ==
        SessionOverlayPhase::Error);
    CHECK(facade.sessionMenuOpen);
    CHECK(
        facade.sessionMenuStatus.find("R2C1") !=
        std::string::npos);
    CHECK(
        facade.sessionMenuStatus.find("fresh") !=
        std::string::npos);
}

void RuntimeJoinRejectsUnsafeGuestSave() {
    TestFacade facade;
    facade.clipboard = "R2C1.private-test";
    facade.sample.missionActive = true;
    TestTransport transport;
    transport.acknowledgeHello = false;
    transport.injectRemoteState = false;
    BridgeRuntime runtime{facade, transport};
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;
    CHECK(runtime.Start(supported, error));

    facade.menuInput.f8 = true;
    runtime.Tick();
    facade.menuInput = {};
    runtime.Tick();
    facade.menuInput.down = true;
    runtime.Tick();
    facade.menuInput = {};
    runtime.Tick();
    facade.menuInput.confirm = true;
    runtime.Tick();

    CHECK(transport.sent.size() == 1U);
    CHECK(
        facade.sessionMenuPhase ==
        SessionOverlayPhase::Error);
    CHECK(
        facade.sessionMenuStatus.find("outside a mission") !=
        std::string::npos);
    CHECK(HasLog(facade, "[WARNING][MISSION_PREFLIGHT]"));

    // The same JOIN selection remains reusable after loading a calm save.
    facade.menuInput = {};
    runtime.Tick();
    facade.sample.missionActive = false;
    facade.menuInput.confirm = true;
    runtime.Tick();
    CHECK(transport.sent.size() == 2U);
    CHECK(
        transport.sent.back().header.type ==
        MessageType::SessionMenuRequest);
    CHECK(
        transport.sent.back().payload.front() ==
        static_cast<std::uint8_t>(
            SessionMenuAction::JoinFromClipboard));
    CHECK(HasLog(facade, "[MISSION_PREFLIGHT] guest local save accepted"));
    CHECK(facade.missionAuthorityActive);

    const std::string joinError{"join rejected"};
    Frame rejected;
    rejected.header.type = MessageType::SessionMenuStatus;
    rejected.payload = {
        static_cast<std::uint8_t>(SessionMenuStatusKind::Error),
        static_cast<std::uint8_t>(joinError.size()),
        0U,
        0U,
        0U};
    rejected.payload.insert(
        rejected.payload.end(),
        joinError.begin(),
        joinError.end());
    transport.inbound.push_back(std::move(rejected));
    facade.menuInput = {};
    runtime.Tick();
    CHECK(!facade.missionAuthorityActive);
}

void RuntimeBothRolesCanTeleportToPeer() {
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;

    TestFacade guestFacade;
    TestTransport guestTransport;
    BridgeRuntime guestRuntime{guestFacade, guestTransport};
    CHECK(guestRuntime.Start(supported, error));
    guestRuntime.Tick();
    CHECK(guestRuntime.LocalSlot() == PlayerSlot::Guest);
    CHECK(guestFacade.realtimeSessionActive);

    InvokeTeleportToPlayerMenu(guestRuntime, guestFacade);
    CHECK(!guestFacade.networkCommands.empty());
    const auto& guestTeleport =
        guestFacade.networkCommands.back();
    CHECK(
        guestTeleport.opcode ==
        CommandOpcode::TeleportGuest);
    CHECK(
        guestTeleport.target ==
        guestRuntime.LocalEntityId());
    CHECK(
        std::abs(
            std::hypot(
                guestTeleport.position.x -
                    guestTransport.remotePosition.x,
                guestTeleport.position.y -
                    guestTransport.remotePosition.y) -
            1.5F) <
        0.0001F);
    CHECK(
        !LastSentCommand(
             guestTransport,
             CommandOpcode::TeleportGuest)
             .has_value());
    CHECK(HasLog(guestFacade, "teleport to player applied locally"));

    TestFacade hostFacade;
    TestTransport hostTransport;
    hostTransport.acknowledgementPayload = {
        static_cast<std::uint8_t>(PlayerSlot::Host)};
    hostTransport.remoteSlot = PlayerSlot::Guest;
    hostTransport.remoteEntityId =
        NetEntityId::Compose(0x0BADF00DU, 2U);
    BridgeRuntime hostRuntime{hostFacade, hostTransport};
    CHECK(hostRuntime.Start(supported, error));
    hostRuntime.Tick();
    CHECK(hostRuntime.LocalSlot() == PlayerSlot::Host);

    InvokeTeleportToPlayerMenu(hostRuntime, hostFacade);
    CHECK(!hostFacade.networkCommands.empty());
    const auto& hostTeleport =
        hostFacade.networkCommands.back();
    CHECK(hostTeleport.target == hostRuntime.LocalEntityId());
    CHECK(HasLog(hostFacade, "teleport to player applied locally"));
}

void RuntimeHostBuildsSafeGuestTeleport() {
    TestFacade facade;
    facade.sample.position = {100.0F, 200.0F, 25.0F};
    facade.sample.heading = 90.0F;
    facade.sample.weaponHash = 0x10203040U;
    facade.sample.weaponAmmo = 36U;
    TestTransport transport;
    transport.acknowledgementPayload = {
        static_cast<std::uint8_t>(PlayerSlot::Host)};
    transport.remoteSlot = PlayerSlot::Guest;
    transport.remoteEntityId =
        NetEntityId::Compose(0x0BADF00DU, 2U);

    BridgeRuntime runtime{facade, transport};
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;
    CHECK(runtime.Start(supported, error));
    runtime.Tick();
    CHECK(runtime.LocalSlot() == PlayerSlot::Host);
    CHECK(runtime.RemoteConnected());
    CHECK(
        std::any_of(
            transport.sent.begin(),
            transport.sent.end(),
            [](const Frame& frame) {
                return frame.header.type ==
                       MessageType::WorldState;
            }));
    const auto localEquipment =
        std::find_if(
            transport.sent.begin(),
            transport.sent.end(),
            [](const Frame& frame) {
                return frame.header.type ==
                       MessageType::EquipmentState;
            });
    CHECK(localEquipment != transport.sent.end());
    const auto decodedLocalEquipment =
        DecodeEquipmentState(localEquipment->payload);
    CHECK(decodedLocalEquipment.has_value());
    CHECK(
        decodedLocalEquipment->weaponHash ==
        facade.sample.weaponHash);

    InvokeTeleportGuestMenu(runtime, facade);

    const auto command =
        LastSentCommand(transport, CommandOpcode::TeleportGuest);
    CHECK(command.has_value());
    CHECK(command->target == transport.remoteEntityId);
    CHECK(Distance(command->position, Vec3{}) > 0.0F);
    CHECK(
        std::abs(
            std::hypot(
                command->position.x - facade.sample.position.x,
                command->position.y - facade.sample.position.y) -
            1.5F) < 0.0001F);
    CHECK(
        std::abs(
            command->position.z - facade.sample.position.z) <
        0.0001F);
    CHECK(
        std::abs(command->heading - facade.sample.heading) <
        0.0001F);
    CHECK(
        std::none_of(
            facade.localCommands.begin(),
            facade.localCommands.end(),
            [](const BridgeCommand local) {
                return local == BridgeCommand::TeleportGuest;
            }));
    CHECK(
        std::none_of(
            facade.networkCommands.begin(),
            facade.networkCommands.end(),
            [](const CommandPayload& local) {
                return local.opcode ==
                       CommandOpcode::TeleportGuest;
            }));
    CHECK(HasLog(facade, "teleport guest queued for entity"));
}

void RuntimeHostRejectsStaleGuestTeleport() {
    TestFacade facade;
    TestTransport transport;
    transport.acknowledgementPayload = {
        static_cast<std::uint8_t>(PlayerSlot::Host)};
    transport.remoteSlot = PlayerSlot::Guest;
    transport.remoteEntityId =
        NetEntityId::Compose(0x0BADF00DU, 2U);

    BridgeRuntime runtime{facade, transport};
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;
    CHECK(runtime.Start(supported, error));
    runtime.Tick();
    facade.tick += 1'001U;

    InvokeTeleportGuestMenu(runtime, facade);

    CHECK(
        !LastSentCommand(
             transport,
             CommandOpcode::TeleportGuest)
             .has_value());
    CHECK(HasLog(facade, "guest stream is stale"));
    CHECK(
        std::none_of(
            facade.localCommands.begin(),
            facade.localCommands.end(),
            [](const BridgeCommand local) {
                return local == BridgeCommand::TeleportGuest;
            }));
}

void RuntimeGuestValidatesTeleportTarget() {
    TestFacade facade;
    TestTransport transport;
    transport.injectRemoteState = false;
    BridgeRuntime runtime{facade, transport};
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;
    CHECK(runtime.Start(supported, error));
    runtime.Tick();
    CHECK(runtime.LocalSlot() == PlayerSlot::Guest);

    InvokeTeleportGuestMenu(runtime, facade);
    CHECK(
        !LastSentCommand(
             transport,
             CommandOpcode::TeleportGuest)
             .has_value());
    CHECK(
        HasLog(
            facade,
            "only the host may request it"));

    Frame matching;
    matching.header.type = MessageType::Command;
    matching.payload = EncodeCommand(
        CommandPayload{
            CommandOpcode::TeleportGuest,
            0U,
            runtime.LocalEntityId(),
            {11.0F, 12.0F, 13.0F},
            45.0F,
            0.0F});
    transport.inbound.push_back(std::move(matching));
    runtime.Tick();
    CHECK(facade.networkCommands.size() == 1U);
    CHECK(
        facade.networkCommands.back().opcode ==
        CommandOpcode::TeleportGuest);
    CHECK(
        facade.networkCommands.back().target ==
        runtime.LocalEntityId());
    CHECK(
        HasLog(
            facade,
            "teleport guest command applied to local guest"));

    Frame mismatched;
    mismatched.header.type = MessageType::Command;
    mismatched.payload = EncodeCommand(
        CommandPayload{
            CommandOpcode::TeleportGuest,
            0U,
            NetEntityId::Compose(0xDEADBEEFU, 2U),
            {21.0F, 22.0F, 23.0F},
            90.0F,
            0.0F});
    transport.inbound.push_back(std::move(mismatched));
    runtime.Tick();
    CHECK(facade.networkCommands.size() == 1U);
    CHECK(
        HasLog(
            facade,
            "target does not match local guest"));

    TestFacade hostFacade;
    TestTransport hostTransport;
    hostTransport.acknowledgementPayload = {
        static_cast<std::uint8_t>(PlayerSlot::Host)};
    hostTransport.injectRemoteState = false;
    BridgeRuntime hostRuntime{hostFacade, hostTransport};
    CHECK(hostRuntime.Start(supported, error));
    hostRuntime.Tick();

    Frame hostRejected;
    hostRejected.header.type = MessageType::Command;
    hostRejected.payload = EncodeCommand(
        CommandPayload{
            CommandOpcode::TeleportGuest,
            0U,
            hostRuntime.LocalEntityId(),
            {31.0F, 32.0F, 33.0F},
            135.0F,
            0.0F});
    hostTransport.inbound.push_back(std::move(hostRejected));
    hostRuntime.Tick();
    CHECK(hostFacade.networkCommands.empty());
    CHECK(
        HasLog(
            hostFacade,
            "local player is not the guest"));
}

void InvalidRoleAcknowledgement() {
    TestFacade facade;
    TestTransport transport;
    transport.acknowledgementPayload = {2U};
    transport.injectRemoteState = false;
    BridgeRuntime runtime{facade, transport};
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;
    CHECK(runtime.Start(supported, error));
    runtime.Tick();
    CHECK(!runtime.LocalSlot().has_value());
    CHECK(!runtime.LocalEntityId().IsValid());
    CHECK(!transport.connected);
    CHECK(transport.sent.size() == 1U);
}

void ReconnectWaitsForFreshRole() {
    TestFacade facade;
    TestTransport transport;
    transport.injectRemoteState = false;
    BridgeRuntime runtime{facade, transport};
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;
    CHECK(runtime.Start(supported, error));
    runtime.Tick();
    CHECK(runtime.LocalSlot() == PlayerSlot::Guest);

    transport.acknowledgeHello = false;
    transport.Disconnect();
    facade.tick += 1'000U;
    runtime.Tick();
    CHECK(transport.connected);
    CHECK(!runtime.LocalSlot().has_value());
    const auto countBefore = transport.sent.size();
    facade.tick += 100U;
    runtime.Tick();
    CHECK(transport.sent.size() == countBefore);

    transport.acknowledgeHello = true;
    transport.Disconnect();
    facade.tick += 1'000U;
    runtime.Tick();
    CHECK(runtime.LocalSlot() == PlayerSlot::Guest);
    CHECK(runtime.LocalEntityId().Counter() == 2U);
}

void RuntimeSynchronizedPauseRequiresBothVotes() {
    TestFacade facade;
    TestTransport transport;
    transport.acknowledgementPayload = {
        static_cast<std::uint8_t>(
            PlayerSlot::Host)};
    transport.remoteSlot = PlayerSlot::Guest;
    BridgeRuntime runtime{facade, transport};
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;
    CHECK(runtime.Start(supported, error));
    runtime.Tick();
    CHECK(runtime.LocalSlot() == PlayerSlot::Host);
    CHECK(facade.realtimeSessionActive);
    CHECK(!facade.sessionMenuOpen);

    facade.menuInput.cancel = true;
    runtime.Tick();
    facade.menuInput = {};
    runtime.Tick();
    auto state = LastSentPauseVote(transport);
    CHECK(state.has_value());
    CHECK(
        state->kind ==
        PauseVoteKind::AuthoritativeState);
    CHECK(
        (state->flags &
         static_cast<std::uint8_t>(
             PauseVoteFlag::HostVoted)) != 0U);
    CHECK(!facade.synchronizedPauseActive);

    Frame guestVote;
    guestVote.header.type = MessageType::PauseVote;
    guestVote.header.sequence =
        ++transport.inboundSequence;
    guestVote.header.tick = facade.tick;
    guestVote.payload = EncodePauseVote(
        PauseVotePayload{
            PauseVoteKind::RequestToggle,
            PlayerSlot::Guest,
            0U,
            state->generation});
    transport.inbound.push_back(
        std::move(guestVote));
    runtime.Tick();
    CHECK(facade.synchronizedPauseActive);
    state = LastSentPauseVote(transport);
    CHECK(state.has_value());
    CHECK(
        (state->flags &
         static_cast<std::uint8_t>(
             PauseVoteFlag::Paused)) != 0U);

    Frame sameSessionResync;
    sameSessionResync.header.type = MessageType::ResyncRequest;
    sameSessionResync.header.sequence = ++transport.inboundSequence;
    sameSessionResync.header.tick = facade.tick;
    transport.inbound.push_back(std::move(sameSessionResync));
    Frame stateAfterResync;
    stateAfterResync.header.type = MessageType::PlayerState;
    stateAfterResync.header.sequence = ++transport.inboundSequence;
    stateAfterResync.header.tick = facade.tick;
    stateAfterResync.payload = EncodePlayerState(
        PlayerStatePayload{
            transport.remoteEntityId,
            transport.remoteSlot,
            PlayerLifecycle::Alive,
            transport.remotePosition,
            {1.0F, 0.0F, 0.0F},
            transport.remoteHeading,
            1.0F,
            0U,
            {},
            0U,
            0.0F,
            0.0F,
            0.0F,
            0.0F,
            transport.remoteLocomotionEpoch});
    transport.inbound.push_back(std::move(stateAfterResync));
    facade.tick += 1U;
    runtime.Tick();
    CHECK(facade.synchronizedPauseActive);

    facade.menuInput.cancel = true;
    runtime.Tick();
    facade.menuInput = {};
    runtime.Tick();
    state = LastSentPauseVote(transport);
    CHECK(state.has_value());
    CHECK(
        (state->flags &
         static_cast<std::uint8_t>(
             PauseVoteFlag::HostVoted)) != 0U);
    CHECK(facade.synchronizedPauseActive);

    Frame guestResume;
    guestResume.header.type = MessageType::PauseVote;
    guestResume.header.sequence =
        ++transport.inboundSequence;
    guestResume.header.tick = facade.tick;
    guestResume.payload = EncodePauseVote(
        PauseVotePayload{
            PauseVoteKind::RequestToggle,
            PlayerSlot::Guest,
            0U,
            state->generation});
    transport.inbound.push_back(
        std::move(guestResume));
    runtime.Tick();
    CHECK(!facade.synchronizedPauseActive);
}

void RuntimeCheckpointRespawnRestoresLifecycle() {
    TestFacade facade;
    TestTransport transport;
    BridgeRuntime runtime{facade, transport};
    const GameIdentity supported{
        std::string{kSupportedExecutableName},
        std::string{kSupportedFileVersion},
        std::string{kSupportedExecutableSha256}};
    std::string error;
    CHECK(runtime.Start(supported, error));
    runtime.Tick();

    facade.sample.downed = true;
    facade.sample.healthFraction = 0.0F;
    runtime.Tick();
    CHECK(
        runtime.Players()
            .State(PlayerSlot::Guest)
            .lifecycle ==
        PlayerLifecycle::Downed);

    facade.sample.downed = false;
    facade.sample.healthFraction = 1.0F;
    runtime.Tick();
    facade.tick += 1'600U;
    runtime.Tick();
    CHECK(
        runtime.Players()
            .State(PlayerSlot::Guest)
            .lifecycle ==
        PlayerLifecycle::Alive);
    CHECK(
        HasLog(
            facade,
            "checkpoint respawn detected"));

    bool sentAlive{};
    for (const auto& frame : transport.sent) {
        if (frame.header.type !=
            MessageType::DownedState) {
            continue;
        }
        const auto lifecycle =
            DecodeDownedState(frame.payload);
        if (lifecycle.has_value() &&
            lifecycle->lifecycle ==
                PlayerLifecycle::Alive) {
            sentAlive = true;
        }
    }
    CHECK(sentAlive);
}

}  // namespace

int main() {
    const std::vector<std::pair<std::string_view, std::function<void()>>> tests{
        {"FrameCodecRoundTrip", FrameCodecRoundTrip},
        {"CampaignMissionCatalogIsExplicitAndBound",
         CampaignMissionCatalogIsExplicitAndBound},
        {"PlayerActionEpochPolicy", PlayerActionEpochPolicy},
        {"GuestMissionGatePolicy", GuestMissionGatePolicy},
        {"PeerMountPullInputPolicy", PeerMountPullInputPolicy},
        {"PayloadContracts", PayloadContracts},
        {"AnimationReplicationPayloadContracts",
         AnimationReplicationPayloadContracts},
        {"SequenceAndEntityIds", SequenceAndEntityIds},
        {"WorldMirrorLifecycle", WorldMirrorLifecycle},
        {"WorldMirrorEntityGraphOrdering",
         WorldMirrorEntityGraphOrdering},
        {"WorldMirrorPriorityBudgetAndHysteresis",
         WorldMirrorPriorityBudgetAndHysteresis},
        {"ReviveStateMachine", ReviveStateMachine},
        {"BubbleBoundaries", BubbleBoundaries},
        {"MenuEdges", MenuEdges},
        {"SessionOverlayAndPayloads", SessionOverlayAndPayloads},
        {"RemoteMotionPlanning", RemoteMotionPlanning},
        {"RemoteSnapshotInterpolation",
         RemoteSnapshotInterpolation},
        {"GateAndTelemetry", GateAndTelemetry},
        {"RuntimeUsesGameDesiredMoveBlendForScriptedSpeed",
         RuntimeUsesGameDesiredMoveBlendForScriptedSpeed},
        {"RuntimeLoopback", RuntimeLoopback},
        {"RuntimeAnimGraphModeIsIndependent",
         RuntimeAnimGraphModeIsIndependent},
        {"RuntimeGuestPipeReconnectPreservesOutboundContinuity",
         RuntimeGuestPipeReconnectPreservesOutboundContinuity},
        {"RuntimeHudFreshnessToleratesInTickClockAdvance",
         RuntimeHudFreshnessToleratesInTickClockAdvance},
        {"RuntimeDefersRestraintUntilRemoteSpawn",
         RuntimeDefersRestraintUntilRemoteSpawn},
        {"RuntimeClearsDeferredRestraintsAtSessionBoundaries",
         RuntimeClearsDeferredRestraintsAtSessionBoundaries},
        {"RuntimeRespawnsUnavailableReplica",
         RuntimeRespawnsUnavailableReplica},
        {"RuntimeDespawnsStaleReplica",
         RuntimeDespawnsStaleReplica},
        {"RuntimeAppliesHostWorldOutsideCutscenes",
         RuntimeAppliesHostWorldOutsideCutscenes},
        {"RuntimeHostEmitsMissionStateTransitions",
         RuntimeHostEmitsMissionStateTransitions},
        {"RuntimeIgnoresNonMissionStoryLoadCamera",
         RuntimeIgnoresNonMissionStoryLoadCamera},
        {"RuntimeHunt1MissionProgressionHandshake",
         RuntimeHunt1MissionProgressionHandshake},
        {"RuntimeHostDebouncesScriptedControlPresentation",
         RuntimeHostDebouncesScriptedControlPresentation},
        {"RuntimeHostSpectatesMinigamesImmediately",
         RuntimeHostSpectatesMinigamesImmediately},
        {"RuntimeVerticalMissionRewardCutsceneReconnect",
         RuntimeVerticalMissionRewardCutsceneReconnect},
        {"RuntimeHostForwardsObservedCampaignCapability",
         RuntimeHostForwardsObservedCampaignCapability},
        {"RuntimeHostStreamsAndStopsMissionCamera",
         RuntimeHostStreamsAndStopsMissionCamera},
        {"RuntimeAnimSceneTransportPrefersNativeAndFallsBackToCamera",
         RuntimeAnimSceneTransportPrefersNativeAndFallsBackToCamera},
        {"RuntimeAnimSceneHybridTwoPhaseCommit",
         RuntimeAnimSceneHybridTwoPhaseCommit},
        {"RuntimeGuestPresentsHostObjectiveAndCutsceneCamera",
         RuntimeGuestPresentsHostObjectiveAndCutsceneCamera},
        {"RuntimeCinematicChainsLoadingAndWaitsForResumeReady",
         RuntimeCinematicChainsLoadingAndWaitsForResumeReady},
        {"RuntimeGuestRejectsOldCameraAndTearsDownOnHostLoss",
         RuntimeGuestRejectsOldCameraAndTearsDownOnHostLoss},
        {"RuntimeCinematicSkipUsesStandardInputWithoutEndingMission",
         RuntimeCinematicSkipUsesStandardInputWithoutEndingMission},
        {"RuntimeStructuredDiagnosticsAreRateLimitedAndCorrelated",
         RuntimeStructuredDiagnosticsAreRateLimitedAndCorrelated},
        {"RuntimeSessionHealthReportsAndResetsHitchWindow",
         RuntimeSessionHealthReportsAndResetsHitchWindow},
        {"RuntimeHostIsolatesRemoteParticipantDuringScenes",
         RuntimeHostIsolatesRemoteParticipantDuringScenes},
        {"RuntimeGuestQuarantinesLocalMissionAndDefersAnchor",
         RuntimeGuestQuarantinesLocalMissionAndDefersAnchor},
        {"RuntimeGuestMissionLeaseSurvivesEverySessionPhase",
         RuntimeGuestMissionLeaseSurvivesEverySessionPhase},
        {"RuntimeGuestAcceptsMissionStateAndGatesWorldGraph",
         RuntimeGuestAcceptsMissionStateAndGatesWorldGraph},
        {"RuntimeHostStreamsAndCleansWorldMirror",
         RuntimeHostStreamsAndCleansWorldMirror},
        {"RuntimeGuestUpsertsAndCleansWorldMirror",
         RuntimeGuestUpsertsAndCleansWorldMirror},
        {"RuntimeHostValidatesGuestDamageIntent",
         RuntimeHostValidatesGuestDamageIntent},
        {"RuntimeScriptOwnedDamageRequiresActiveHostMission",
         RuntimeScriptOwnedDamageRequiresActiveHostMission},
        {"RuntimeWorldMirrorUsesHostMissionAuthority",
         RuntimeWorldMirrorUsesHostMissionAuthority},
        {"RuntimeDamageIntentToleratesReorderAndKeepsGuards",
         RuntimeDamageIntentToleratesReorderAndKeepsGuards},
        {"RuntimeHostEnforcesLifecycleAuthority",
         RuntimeHostEnforcesLifecycleAuthority},
        {"RuntimeResyncEquipmentRespectsRoleAuthority",
         RuntimeResyncEquipmentRespectsRoleAuthority},
        {"RuntimeSessionOverlay", RuntimeSessionOverlay},
        {"RuntimeBothRolesCanTeleportToPeer",
         RuntimeBothRolesCanTeleportToPeer},
        {"RuntimeHostBuildsSafeGuestTeleport",
         RuntimeHostBuildsSafeGuestTeleport},
        {"RuntimeHostRejectsStaleGuestTeleport",
         RuntimeHostRejectsStaleGuestTeleport},
        {"RuntimeGuestValidatesTeleportTarget",
         RuntimeGuestValidatesTeleportTarget},
        {"InvalidRoleAcknowledgement", InvalidRoleAcknowledgement},
        {"ReconnectWaitsForFreshRole", ReconnectWaitsForFreshRole},
        {"RuntimeSynchronizedPauseRequiresBothVotes",
         RuntimeSynchronizedPauseRequiresBothVotes},
        {"RuntimeCheckpointRespawnRestoresLifecycle",
         RuntimeCheckpointRespawnRestoresLifecycle},
    };

    std::size_t failures{};
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "[PASS] " << name << '\n';
        } catch (const std::exception& exception) {
            ++failures;
            std::cerr << "[FAIL] " << name << ": "
                      << exception.what() << '\n';
        }
    }
    std::cout << tests.size() - failures << "/" << tests.size()
              << " bridge self-tests passed.\n";
    return failures == 0U ? 0 : 1;
}
