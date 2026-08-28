# Current state

This is an advanced replication prototype, not yet a complete playable RDR2 Story co-op mod. Player/world networking and experimental mission presentation exist, but Rockstar's Story script runtime, saves, AI, physics, and campaign state are not fully synchronized.

## Confirmed foundations

| Area | State | Notes |
| --- | --- | --- |
| Private host/guest connection, authentication, reconnect | Implemented | Versioned TCP/UDP protocol with host authority and bounded payloads. |
| Remote player, traversal, actions, basic combat | Implemented foundation | Replicated movement is not deterministic physics. |
| Nearby host world entities | Partial | Spawn/update/despawn proxy graph; it is not a full AI/physics simulation. |
| Separate player mounts | Partial | Basic mount replication preserves peer ownership; stable, bonding, cargo, and death are private/unimplemented. |
| Shared wagon/carriage | Experimental | The host drives its local wagon. The guest can hold the normal interaction key near the host driver to request the passenger seat; host authority validates the request and each game seats its local/replica ped in the corresponding wagon. Vehicle physics, horses, damage, trains, boats, and mission-owned vehicles remain unverified. |
| Crafting/cooking activity presentation | Experimental | Each player uses their own vanilla crafting UI and keeps their own ingredients, products, money, and progress. A detected free-roam scenario publishes a host-authoritative activity action; the remote puppet uses the nearest matching local scenario at that anchor when one exists. Exact recipe selection, crafting menus, and mission-owned crafting remain local/unverified. |
| Mission state, objective, camera, checkpoint transport | Experimental | The host alone publishes the active mission epoch, anchor, phase and companion objective marker. The guest follows that presentation without starting or unlocking the host Story mission. A conflicting guest-local Story mission is quarantined, with an in-game warning; it cannot replace host authority. Checkpoint retry is host-only and explicitly chosen, but the underlying RDR2 retry native remains unverified. |
| Per-mission progression handshake | Experimental | Protocol 27 sends the host's exact catalog mission ID, requires a guest-local preflight, then applies the host's verified MissionData completion rating (`2` normal / `3` bronze / `4` silver / `5` gold). It transfers the bounded positive cash-balance delta from that exact host mission run and applies only that mission's explicit, idempotent catalogue rewards; the event is consumed only after every verified guest write succeeds. The catalog includes: `WNT4` Carbine Repeater/Lasso; `MUD1` Tomahawk gunsmith eligibility; `SEN1` Tomahawk; `DST1` Double-Barreled Shotgun/Throwing Knives; `MUD4` Rolling Block Rifle; `TRE1` rare Rolling Block Rifle; `FUD1` Fishing Rod; `HNT1` Legendary Animals map/challenge unlock; `GRY1` Evans Repeater gunsmith eligibility; `IND3` Semi-Auto Shotgun; `SAD3` Carcano Rifle; `MAR8` Binoculars fallback; and `AB21` Sadie's telegram. `RABI1` / **A Fisher of Men** lends then removes its fishing rod, so it is not a permanent reward. Documents and future pamphlet/recipe records use the public Character-GUID inventory path; horses and unobserved weapons/unlocks/recipes remain unsupported until individually mapped and tested. Unknown scripts and failed preflight remain companion-only. |
| Cutscene/loading spectator | Implemented foundation | Guest spectator is entered for host cinematic/cutscene presentation. |
| Scripted control-lock spectator | Partial | Camera/fade and the explicit RDR2 minigame signal enter immediately. Mission-owned control loss (including forced AnimScenes, QTE-style input locks, and scripted vehicle/horse entry) requires a 150 ms confirmation; release remains held for 650 ms. The wire state distinguishes scripted vehicle and minigame transitions. Exact QTE and AnimScene/task ownership still need live-game validation. |
| Safe return near host | Partial | Deferred guest return now asks RDR2 for a nearby collision/navmesh-safe pedestrian coordinate (bounded to 6 m), then falls back to conservative ground-height correction. Interior, moving train/wagon/boat, hostile zone, and horse-placement cases remain live manual-test gates. |
| Weapon-shop entitlement | Proven/manual test; acquisition observer live-test pending | Repeating Shotgun `UNLOCK` test makes the item purchasable without granting ownership, money, or ammo. During an active host session, a baseline false→true `HAS_PED_GOT_WEAPON` transition for that exact proven weapon now produces the existing capability-journal event without requiring equip; it still needs a live Story reward test. |
| Recipe entitlement | Proven locally; guest handoff unproven | On 2026-08-28, the guarded Story Mode Poison Throwing Knife test changed `visible=0->1, unlocked=0->1` and a re-probe returned `1->1`. This was one host game with a synthetic guest, not a two-PC guest-save test. |
| Capability journal | Implemented, deny-by-default | Host-only, atomic JSON + backup journal for shared capabilities; reconnect replay and idempotency are automated. Only the proven Repeating Shotgun and Poison Throwing Knife records may be persisted, forwarded, or applied; unknown records are logged and rejected. It stores no player save, wallet, inventory, horse, or health state. |
| Inventory authority model | Implemented | Per-player claims, transaction rollback, reconnect snapshots, and atomic host claim-cursor persistence are implemented. It never represents RDR2 cash or item inventory. |
| Vanilla pickup-to-inventory callback | Partial; live test pending | The bridge enumerates active vanilla pickups and reports only a positive `HAS_PICKUP_BEEN_COLLECTED` transition. Authenticated host authority records per-player, zero-value claims keyed by pickup hash plus quantized world position. It deliberately copies no money, consumables, or items. Corpse/container/plant/document mapping remains unsupported. |

## Reward policy

| Reward | Guest result | Rule |
| --- | --- | --- |
| Mission-granted weapon | Conditional | Grant the guest's own item only when the exact completed mission has a reviewed, idempotent catalogue record. |
| Weapon shop unlock | Conditional | Replicate a reviewed shop entitlement; ownership/ammo remain private unless that mission has a separate reviewed ownership record. |
| Satchel, tonic upgrade, recipe | Conditional | Apply only reviewed mission/capability records. Unknown records are logged, never copied. |
| Mission cash award | Conditional | After the matching verified completion succeeds, transfer that mission run's bounded positive host cash-balance delta once. |
| Free-roam money and consumables | No | Each player receives, spends, loots, and saves their own local results; corpse and world loot are never copied. |
| Horse reward/progression | Usually no | Never overwrite guest horse, stable record, bonding, cargo, or cores. |

## Important limits

- No savegame merging or active-save replacement is part of the runtime.
- A guest can join a host in Chapter 2 even if the guest save is at Chapter 3 (or has not unlocked the host mission): no guest campaign progress is overwritten or unlocked unless the exact active mission is in the catalog, the guest confirms it is startable in that save, and MissionData accepts the normal-complete mapping. Otherwise it remains companion-only. The guest must not start a private Story mission while connected. If they do, that local mission is quarantined and the HUD tells them to exit it before following the host again.
- A player being downed does not automatically discard a checkpoint. The living peer can use the normal hold-to-revive interaction. If a retry is needed, only the host can choose **Retry checkpoint** from the diagnostic menu; the command is host-authoritative, but it is still a native-validation/test path rather than a proven RDR2 checkpoint retry.
- Completing a host mission does **not** yet generically detect and replicate every unlock; the journal only makes an observed capability durable. A bounded host-only ScriptHook event observer records event group/index/type metadata for discovery. Separately, the proven Repeating Shotgun capability is promoted only from a baseline false-to-true host ownership transition during an active co-op session, so no equip action is required. Other event schemas and all other records remain diagnostic-only and are never emitted.
- Map pickup collection has a conservative game-event source and a durable co-op claim cursor, but live Story Mode verification is still pending. Corpse loot stays vanilla and private: host and guest should each be able to loot the same corpse on their own machine and receive the normal local reward. No corpse-specific tracker, money copy, or item copy is needed unless a two-PC test shows a visibility/reset failure. Plants, drawers, documents, crafting materials, pelts, carcasses, and cash still need their own verified local event sources.
- Full mission co-op, deterministic animals/NPC AI, shared vehicle physics, law, audio, card games, recipe/menu synchronization, trains, boats, and a verified native checkpoint-retry binding remain future work.

## Automated verification (2026-08-28)

The two-save progression test procedure is in
[`MISSION_PROGRESSION_TEST_MATRIX.md`](MISSION_PROGRESSION_TEST_MATRIX.md).

- Native bridge build and `ctest`: **1/1 passed**.
- Native vertical bridge regression: host mission start, proven weapon-capability observation, cutscene state, bridge reconnect, and cutscene-state replay are exercised in one sequence. This is automated runtime coverage, not a substitute for a two-PC Story mission completion.
- Managed protocol/sidecar self-test: **49/49 passed**.
- Launcher self-test: **28/28 passed**.
- Manual Story Mode bridge-load test: **passed**. ScriptHook registered and launched the fresh v23 bridge; the live HUD showed `COOP HOST | IPC CONNECTED | REMOTE STREAMING` and the F9 menu opened/closed normally. No Story progress was saved.
- Manual Story Mode recipe test: **passed locally** (`0->1`, then `1->1` re-probe); exit this disposable session **without saving**. Guest-native application, ordinary-save persistence, and two-PC replay remain unproven.

The next high-value manual test is a controlled mission with a cutscene, control-lock release, checkpoint, and reconnect. The next entitlement test must use a real guest game to verify native application, acknowledgement, reconnect replay, and normal save/restart persistence.
