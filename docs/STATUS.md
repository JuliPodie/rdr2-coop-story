# Current state

This is an advanced replication prototype, not yet a complete playable RDR2 Story co-op mod. Player/world networking and experimental mission presentation exist, but Rockstar's Story script runtime, saves, AI, physics, and campaign state are not fully synchronized.

## Confirmed foundations

| Area | State | Notes |
| --- | --- | --- |
| Private host/guest connection, authentication, reconnect | Implemented | Versioned TCP/UDP protocol with host authority and bounded payloads. |
| Remote player, traversal, actions, basic combat | Implemented foundation | Replicated movement is not deterministic physics. |
| Nearby host world entities | Partial | Spawn/update/despawn proxy graph; it is not a full AI/physics simulation. |
| Separate player mounts | Partial | Basic mount replication preserves peer ownership; stable, bonding, cargo, and death are private/unimplemented. |
| Mission state, objective, camera, checkpoint transport | Experimental | Host state and presentation are transported; Story scripts are still local. |
| Cutscene/loading spectator | Implemented foundation | Guest spectator is entered for host cinematic/cutscene presentation. |
| Scripted control-lock spectator | Partial | Host samples control loss, screen transitions, scenario/table use and vehicle entry; release is debounced for 650 ms. QTE and AnimScene/task ownership still need proven game hooks. |
| Safe return near host | Partial | Deferred safe-ground placement exists. Interior, moving train/wagon/boat, hostile zone, and horse-placement cases remain manual-test gates. |
| Weapon-shop entitlement | Proven/manual test | Repeating Shotgun `UNLOCK` test makes the item purchasable without granting ownership, money, or ammo. |
| Recipe entitlement | Proven locally; guest handoff unproven | On 2026-08-28, the guarded Story Mode Poison Throwing Knife test changed `visible=0->1, unlocked=0->1` and a re-probe returned `1->1`. This was one host game with a synthetic guest, not a two-PC guest-save test. |
| Capability journal | Implemented, deny-by-default | Host-only, atomic JSON + backup journal for shared capabilities; reconnect replay and idempotency are automated. Only the proven Repeating Shotgun and Poison Throwing Knife records may be persisted, forwarded, or applied; unknown records are logged and rejected. It stores no player save, wallet, inventory, horse, or health state. |
| Inventory authority model | Implemented in isolation | Per-player claims, transaction rollback, reconnect snapshots and persistence are tested. |
| Vanilla pickup-to-inventory callback | Missing | The SDK can query a known pickup but provides no safe universal vanilla pickup enumeration/callback or reward-value payload. No real game pickup currently calls `ClaimLoot`. |

## Reward policy

| Reward | Guest result | Rule |
| --- | --- | --- |
| Mission-granted weapon | Capability yes | Grant guest's own entitlement once an authoritative mission reward event is proven. |
| Weapon shop unlock | Capability yes | Replicate shop eligibility; ownership/ammo remain private. |
| Satchel, tonic upgrade, recipe | Capability yes | Apply only proven live entitlement records. Unknown records are logged, never copied. |
| Consumables and money | No | Each player receives, spends, loots, and saves their own local results. |
| Horse reward/progression | Usually no | Never overwrite guest horse, stable record, bonding, cargo, or cores. |

## Important limits

- No savegame merging or active-save replacement is part of the runtime.
- Completing a host mission does **not** yet generically detect and replicate every unlock; the journal only makes an observed capability durable.
- All map loot, corpse loot, plants, drawers, documents, crafting materials, pelts, carcasses, and cash remain unsynchronized until a verified game event source identifies the exact local pickup.
- Full mission co-op, deterministic animals/NPC AI, physics, law, audio, card games, crafting activities, and vehicles remain future work.

## Automated verification (2026-08-28)

- Native bridge build and `ctest`: **1/1 passed**.
- Managed protocol/sidecar self-test: **48/48 passed**.
- Launcher self-test: **28/28 passed**.
- Manual Story Mode recipe test: **passed locally** (`0->1`, then `1->1` re-probe); exit this disposable session **without saving**. Guest-native application, ordinary-save persistence, and two-PC replay remain unproven.

The next high-value manual test is a controlled mission with a cutscene, control-lock release, checkpoint, and reconnect. The next entitlement test must use a real guest game to verify native application, acknowledgement, reconnect replay, and normal save/restart persistence.
