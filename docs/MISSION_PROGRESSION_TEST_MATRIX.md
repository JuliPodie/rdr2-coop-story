# Campaign progression two-save validation

For the separate mission-specific local-dialogue capture and admission
contract, see [MISSION_DIALOGUE_PROFILES.md](MISSION_DIALOGUE_PROFILES.md).

Use two disposable Story Mode saves. The host must start each mission normally.
The guest must have the **same exact mission** incomplete and startable. Do not
test from a completed guest mission or an Online session.

## Matching-instance barrier (two-PC required)

Protocol 32 uses a 45-second, exact-MissionData start window. It keeps the
guest prompt blocked for the first 250 ms while it verifies that the guest was
idle, so an independently started local mission cannot be adopted. This is a
separate live gate from completion/reward validation and must be tested before
calling a mission shared-play ready:

1. Start both games on disposable saves where `FUD1` or `HNT1` is incomplete
   and locally startable for both players.
2. Let the host start the mission normally. Confirm the host log contains an
   `offer` followed by `host mission active; exact guest start window opened`.
3. On the guest, use only the matching vanilla mission prompt. Confirm
   `guest entered exact local MissionData instance` and then `host
   acknowledged matching guest mission instance` in the two logs.
4. Repeat while the guest deliberately does nothing for 45 seconds: the host
   must log the timeout and the guest must remain companion-only, without a
   black screen or permanently blocked normal conversations.
5. Repeat with a different available guest Story mission. It must log a
   rejected matching instance and return to quarantine; it must never become
   the host mission or write guest progress.
6. After a successful release, have the host choose **Retry checkpoint**.
   The guest must receive that retry only while its exact matching MissionData
   entry is still active. Repeat outside a released barrier and verify the
   guest log says the retry was ignored.
7. Race test: let the guest begin the matching Dutch prompt just before the
   host starts it, then let the host barrier arrive. The guest must log that
   its matching local mission was already entering, send a rejection, and
   remain isolated. It must not produce a `guest entered exact local
   MissionData instance` message.

The current barrier authorizes only the matching vanilla start. The host can
relay bounded cached objective text for matching guest presentation, but it
does not yet prove synchronized checkpoint script state or dialogue VM timing;
capture those observations separately rather than treating a clean barrier
release as campaign-co-op completion.

## Registry coverage

The catalog contains every one of the 79 MissionData Story entries registered
by the pinned game's `init_all_sp` script, plus `RABI1` (the separately
registered Fisher of Men entry): 80 exact mission IDs in total. Completion and
rating writes and MissionData-derived unlocks are enabled for all 80 after an
exact released guest instance. This includes vanilla chapter, activity,
encounter, recipe-availability, and shop gates which query the completed
MissionData record. The 17 source-reviewed entries below additionally apply their concrete
idempotent item, weapon, document, or retail-entitlement records. The other 63
never synthesize an extra direct reward beyond MissionData completion/rating
and its derived unlocks.

For every row, capture both players' Mission Replay rating before the
host accepts the mission. After completion, wait for the guest bridge log to
contain `MISSION_PROGRESSION` and save/reload both games before recording the
result.

| Host mission | Guest must gain | Verify |
| --- | --- | --- |
| `WNT4` Old Friends | Carbine Repeater, Lasso | Both are owned; the guest's replay rating equals the host's. |
| `MUD1` Americans at Rest | Tomahawk shop entitlement | Tomahawk is purchasable, but not owned for free. |
| `SEN1` The First Shall Be the Last | Tomahawk | Guest owns a Tomahawk. |
| `DST1` Paying a Social Call | Double-Barreled Shotgun, Throwing Knives | Both weapon records are owned. |
| `MUD4` The Sheep and the Goats | Rolling Block Rifle | Guest owns the standard Rolling Block Rifle. |
| `TRE1` Magicians for Sport | Rare Rolling Block Rifle | Guest owns the exotic Rolling Block record. |
| `UTP2` An American Pastoral Scene | Lancaster Repeater | Guest owns the Lancaster Repeater. |
| `MUD6` Pouring Forth Oil | Pump-Action Shotgun shop entitlement | Pump-Action Shotgun is purchasable, but not owned for free. |
| `FUD1` The New South | Fishing Rod | Guest owns the Fishing Rod. |
| `HNT1` Exit Pursued by a Bruised Ego | Legendary Animals map, hunting gate | Map appears in inventory; hunting gate is visible. |
| `GRY1` American Distillation | Evans Repeater shop entitlement | Evans Repeater is purchasable, but not owned for free. |
| `IND3` A Fine Night of Debauchery | Semi-Auto Shotgun and High Roller Revolver shop entitlements; Reutlinger watch | Both weapons are purchasable rather than free; the watch appears in guest inventory. |
| `GNG3` Visiting Hours | Repeating Shotgun shop entitlement | Repeating Shotgun is purchasable, but not owned for free. |
| `DST5` Goodbye, Dear Friend | Carcano Rifle; Litchfield Repeater shop entitlement | Guest owns the Carcano and can purchase the Litchfield. |
| `SAD3` Uncle's Bad Day | Carcano Rifle | Guest owns it. |
| `AB21` The Tool Box | Sadie's telegram | Document appears in guest inventory. |
| `MAR8` American Venom | Binoculars fallback | Guest owns Binoculars if the item was absent. |

## Latest live evidence

- `FUD1` host-side detection and completion passed on 2026-08-28 using the
  disposable `host-before` save. The bridge published the exact `FUD1` offer,
  observed completion, and returned to the idle mission state without a
  cinematic/resume-barrier stall.
- This was a one-PC synthetic-peer run, so it deliberately produced
  `completed with no eligible guest; companion-only retained`. It proves host
  recognition and the safe no-guest path only; it does **not** prove a guest
  save received the fishing rod, rating, or cash.

## Verified-source boundary

The mapping intentionally covers only permanent rewards or availability gates
whose exact game record is known. Mission props, temporary loadouts, optional
pickups, honor, and keepsakes whose persistent inventory record has not been
identified are not synthesized. In particular, `DST5`'s Mary keepsakes remain
outside the automatic mapping until their item-database records are proven.
The compiled catalog and its tests are the source of truth for what will
actually be written to a guest save.

Recipe pamphlets need a separate distinction: most are world loot, purchases,
or companion/Stranger-activity rewards, rather than rewards for a Story
MissionData record. Those remain independent just like world loot. A recipe
that merely becomes buyable after a mission must follow the guest's locally
completed MissionData gate; it must not be injected as a free pamphlet. Add a
`RecipeUnlock` plus its exact `InventoryItem` only when a Story mission is
proven to award that recipe outright.

## Invariants for every row

- Host and guest ratings match exactly: `2` normal, `3` bronze, `4` silver, or
  `5` gold.
- Guest cash does not change. Total-wallet movement is not an authoritative
  mission reward receipt and is never replicated.
- The host retries the same completion transaction until it receives the
  guest's `Applied` acknowledgement. A lost completion or acknowledgement
  therefore cannot silently skip a guest save update; every replay retains
  the same event ID and remains idempotent.
- Repeating the already-consumed completion event changes no guest inventory
  or entitlement.
- If the guest cannot start the same mission, their save receives no MissionData
  update, money, item, weapon, or unlock. The session remains companion-only.
- World/corpse loot, horse state, general inventory, and unrelated free-roam
  cash remain private to each save.

## Failure capture

Attach both bridge logs, the exact mission ID, host and guest ratings, and
screenshots of the relevant inventory or gunsmith page.
Do not edit the `.sav` files while investigating; use copies from
`RDR2-CoopSaveTest` instead.
