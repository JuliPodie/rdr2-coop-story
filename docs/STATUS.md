# Current state — Protocol 32 tester build

RDR2 Coop Story is an experimental private two-PC Story Mode build. It is not a
complete shared campaign: each game retains its own Rockstar script runtime,
save, AI, physics, law, inventory, and world persistence. The current package
is intended to expose as much safe, bounded behaviour as possible to testers
while failing closed when an exact local state cannot be verified.

## Implemented tester systems

| Area | State | Tester-facing behaviour |
| --- | --- | --- |
| Private host/guest transport | Implemented | Authenticated TCP/UDP session, reconnect, role negotiation, bounded protocol parsing, and diagnostics. |
| Player, combat, actions, and AnimGraph | Experimental | Remote player presentation, bounded movement/heading correction, AnimGraph replica, and validated combat/action intents. Physics is not deterministic. |
| World/mount/vehicle/crafting presentation | Experimental | Host-owned proxy graph; separate player mounts; guarded wagon passenger requests; and matching local free-roam crafting/cooking scenarios. Items, ingredients, vehicle physics, horse data, and menus remain private. |
| Downed/revive | Experimental, native live gate | Normal hold-to-revive prompt, 4 s hold, 2 m range, host validation, and 35% local health restore. A retry is never automatic. |
| Mission authority and companion presentation | Experimental | The host publishes mission phase, anchor, objective, camera, and cinematic state. A guest outside a matching instance is kept companion-only and is returned through guarded presentation barriers. |
| Matching MissionData barrier | Implemented, two-PC validation needed | The host offers an exact mission ID. A guest may use only its own identical, incomplete, startable vanilla prompt during the 45 s window. Any conflict, refusal, or timeout stays companion-only. |
| Mission completion/progression | Implemented, two-PC persistence validation needed | For an eligible matching run, the guest can receive verified MissionData completion/rating, bounded positive cash delta, and only reviewed idempotent rewards. The catalog covers the 80 registered Story MissionData entries; item/entitlement writes remain explicitly allow-listed. |
| Mission dialogue coordination | Experimental | The host samples admitted local dialogue roots/lines, sends epoch- and checkpoint-scoped cues, and waits for guest readiness. A matching guest keeps its own vanilla playback; a companion-only guest can use the small reviewed bridge-owned audio presentation. Unmapped dialogue remains local. |
| Ambient encounters | Implemented tester coverage, two-PC validation needed | 94 reviewed action-script detections map to five host-owned profiles: 65 hostile roadside actions, 15 rescues, 6 wagon defenses, 3 animal attacks, and 5 camp clear-outs. The host owns phases/outcome/cleanup; guest combat contributes through validated intent. Non-action ambient vignettes remain visible through the world mirror without inventing combat or rewards. |
| Capability/pickup journal | Guarded and deny-by-default | Reviewed entitlements and collection telemetry are idempotent; unsupported record shapes are rejected. Corpse/container/plant/document inventory is not synchronized. |

## Mission progression and rewards

The guest never receives a host save or a generic "complete this mission"
command. A completion can be applied only when all of the following are true:

1. The host and guest use the same exact catalog MissionData entry.
2. The guest proved that entry incomplete and locally startable before the
   mission began.
3. The completion references that same approved mission epoch and event ID.
4. The guest verifies its own MissionData completion write.
5. Any item, weapon, document, recipe, or shop entitlement has a specific
   reviewed idempotent catalog mapping.

Ratings and a capped positive mission-run cash delta use the same identity and
are applied at most once. Unknown rewards, temporary equipment, horse state,
Honor, optional pickups, and world loot are not synthesized. The detailed
two-save procedure and current explicit rewards are in
[MISSION_PROGRESSION_TEST_MATRIX.md](MISSION_PROGRESSION_TEST_MATRIX.md).

## Ambient encounter policy

An encountered Rockstar script is only local evidence that the bridge may offer
one of the five known profiles. It does not run on the peer. The host validates
distance, mission/cinematic safety, a bounded anchor, session readiness, and
the absence of another active encounter before it creates a shared instance.
The host then creates the physical roster, accepts guest combat contribution,
and resolves or abandons the activity. Terminal scenes are retained briefly so
each game can use normal local interaction, then bridge-created actors are
cleaned up.

No ambient profile synchronizes the original event script, law response,
wagon/horse physics, dialogue, Honor, cash, rewards, collectibles, or campaign
progress. Each local game may offer ordinary generic corpse loot; it stays
local and is never copied. The Valentine Extortion adaptation adds a guest
eligibility preflight but follows the same generic-local-loot policy.

## Important limits

- No savegame merge, active-save replacement, matchmaking, relay, or public
  server exists.
- Rockstar mission execution, checkpoint script state, AI, animals, physics,
  trains, boats, card games, native minigames, and most scripted audio are not
  deterministic across games.
- A guest should not start an unrelated private Story mission while connected.
  The bridge quarantines it rather than trying to merge two mission states.
- The native ASI integration is an explicit private tester configuration built
  with `COOPSTORY_ENABLE_UNVERIFIED_NATIVE_BINDINGS=ON`; it remains a live-game
  validation surface, not a broadly supported compatibility guarantee.
- Story Mode only. Do not disable the online, version, transport, or launcher
  install-ownership gates. Remove project-owned files before opening Red Dead
  Online.

## Latest verification

The Protocol 32 source and tester package were built on 2026-08-30.

- Native ASI build and `ctest`: **1/1 passed**.
- Managed protocol/sidecar self-test: **49/49 passed**.
- Launcher self-test: **28/28 passed**.
- Package validation: **471 files**, no forbidden archive entries, active
  native-binding marker present.
- One-PC disposable-save mission test proved host `FUD1` detection and safe
  completion/no-eligible-guest handling. It did **not** prove two-PC guest
  progression, reward persistence, dialogue, revive, or ambient encounters.

Use [TESTING.md](TESTING.md) for the current test order and stop conditions.
