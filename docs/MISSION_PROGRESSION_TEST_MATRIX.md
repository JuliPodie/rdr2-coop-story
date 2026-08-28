# Campaign progression two-save validation

Use two disposable Story Mode saves. The host must start each mission normally.
The guest must have the **same exact mission** incomplete and startable. Do not
test from a completed guest mission or an Online session.

For every row, capture both players' Mission Replay rating and cash before the
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
| `FUD1` The New South | Fishing Rod | Guest owns the Fishing Rod. |
| `HNT1` Exit Pursued by a Bruised Ego | Legendary Animals map, hunting gate | Map appears in inventory; hunting gate is visible. |
| `GRY1` American Distillation | Evans Repeater shop entitlement | Evans Repeater is purchasable, but not owned for free. |
| `IND3` A Fine Night of Debauchery | Semi-Auto Shotgun | Guest owns it; Reutlinger watch remains an expected gap. |
| `SAD3` Uncle's Bad Day | Carcano Rifle | Guest owns it. |
| `AB21` The Tool Box | Sadie's telegram | Document appears in guest inventory. |

## Invariants for every row

- Host and guest ratings match exactly: `2` normal, `3` bronze, `4` silver, or
  `5` gold.
- The guest receives the host's positive cash-balance delta from that mission
  once only. Reconnect/reload must not duplicate it.
- Repeating the already-consumed completion event changes no guest inventory,
  entitlement, or cash value.
- If the guest cannot start the same mission, their save receives no MissionData
  update, money, item, weapon, or unlock. The session remains companion-only.
- World/corpse loot, horse state, general inventory, and unrelated free-roam
  cash remain private to each save.

## Failure capture

Attach both bridge logs, the exact mission ID, host/guest pre/post cash, host
and guest ratings, and screenshots of the relevant inventory or gunsmith page.
Do not edit the `.sav` files while investigating; use copies from
`RDR2-CoopSaveTest` instead.
