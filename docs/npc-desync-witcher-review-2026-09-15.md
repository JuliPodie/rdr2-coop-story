# NPC desync review — RDR2 and WitcherOnline

## Implementation update

The four concrete playback defects below have now been addressed in the working tree:

- Combat NPCs use their sampled trajectory for movement and a combined move/aim task with firing disabled. Their enemy position is used only for aiming.
- Ordinary root correction operates below the old 12 m threshold and limits soft correction to 4 m/s. Large local tracking errors remain smooth; an implausible change in the authoritative target permits a snap. The existing cinematic 3 m snap behavior is retained. Prediction ends after 250 ms, at which point native navigation is stopped as well.
- NPC mounting/dismounting checks actual native attachment, retries at 500 ms intervals, waits for missing parents, and blocks competing root/task updates during transitions. Dead actors are not remounted or continuously repositioned.
- Task signatures include movement speed, moving/stopped state, aim target, and stale state. Changes are deduplicated with a minimum refresh interval; a bounded watchdog recovers interrupted tasks. Proxy handle replacement resets task and mount caches.

The SDK-free policy is in `src/CoopStory.Bridge/include/coopstory/bridge/WorldProxyPolicy.hpp` and is used by the SDK facade. Three regression groups cover motion convergence, stale prediction, mount failure/retry, and task refresh. Two pre-existing test calls to the removed `ComputeDirectReplicaVisualTaskDestination` API were migrated to the already-present `PlanRemoteAnimGraphMotion` API so the full suite could build.

Validation: `cmake --build --preset bridge-asi-vs2026-release` succeeded, including the SDK-enabled ASI, and `ctest --preset bridge-asi-vs2026-release` passed all **76/76** bridge self-tests. The ASI is at `build/bridge-asi-vs2026/src/CoopStory.Bridge/Release/CoopStoryBridge.asi`. It has not been installed into a game directory or validated in a two-client session.

A subsequent change adds the protocol-34 NPC action journal and SDK-observable playback path described in [NPC action replication](npc-action-replication.md), including its remaining clip-fidelity gaps. A buffered per-NPC interpolation timeline is still not introduced: soft tracking uses the latest accepted snapshot and bounded prediction. The review below records the original findings and their original line locations.

## Scope and evidence

Reviewed the current RDR2 working tree at base commit `3af32e44ad09dade55dcc1452cb9fbc7c9e241d9`, including pre-existing uncommitted changes. No implementation files were changed for this review. WitcherOnline reference: commit `4755ee753dc66a780ae4a72ce2b75f4fecf5c0f4`.

The [35-second Medal clip](https://medal.tv/games/red-dead-2/clips/nqQozaZ38D2PgN_wj) was played and sampled visually. It shows the COOP GUEST / IPC CONNECTED / REMOTE STREAMING overlay, combat in Valentine, a mounted NPC at the opening, a rearing horse around 12 seconds, and armed NPCs near the porch around 23–25 seconds. This is one guest view: it does not establish the host's corresponding positions, entity identities, network delay, or loaded ASI build. The defects below are established by code inspection; attributing a particular visible actor's behavior to one defect requires a matching host/guest capture and build ID.

The screenshot is prior feasibility context. Its highlighted animation and riding concepts are relevant, but [WitcherOnline explicitly does not implement NPC sync](https://github.com/rejuvenate7/WitcherOnline/blob/4755ee753dc66a780ae4a72ce2b75f4fecf5c0f4/README.md). Its remote-player implementation is an architectural reference, not proof that its approach solves RDR2 world replication.

## Findings, in repair order

All SDK line references below refer to `src/CoopStory.Bridge/sdk/ScriptHookSdkFacade.cpp` in the reviewed working tree.

### 1. Combat state suppresses movement replication — high priority

**Functions:** `SampleWorldEntities` (6030–6046) and `MaintainWorldMirrorGuest` (14941–15066).

The sampler classifies an NPC with a player combat target as `Combat`, even when its velocity is nonzero. It also changes `taskTarget` to the combat target's position. The guest explicitly excludes `Combat` from the movement-task branch and only issues `TASK_AIM_GUN_AT_ENTITY` periodically. There is no equivalent continuous movement correction for ordinary combat actors.

**Consequence:** a host NPC can advance, retreat, or strafe while its guest proxy stands aiming or retains a previously issued task. This occurs even with perfectly ordered, lossless packets. The 12 m correction threshold in finding 2 then produces catch-up jumps.

**Repair:** represent locomotion independently from combat/aim state. Follow the authoritative NPC trajectory while maintaining a separate aim overlay. Do not use the enemy's position as the NPC's movement destination, and retain host-only damage authority.

### 2. The ordinary moving-NPC correction path has a 12 m dead zone — high priority

**Functions:** `UpdateWorldEntityProxy` (14009–14010), `MaintainWorldMirrorGuest` (14705–14799). Constant: `kWorldProxySnapDistanceMeters` (153).

Updates overwrite one state and its arrival time. Prediction extrapolates that state for at most 250 ms; there is no per-NPC snapshot interpolation history here. For a non-cinematic, non-idle, unmounted actor, the correction condition is effectively `error >= 12`. Inside that branch, smoothing requires `error < 12`, which cannot be true for that actor class. Thus ordinary moving actors receive no soft correction and jump when the threshold is crossed.

**Consequence:** inaccurate local navigation can diverge by several metres without correction. Horses follow this same NPC path. The 250 ms prediction cap also does not stop an already-running nav task when updates become stale.

**Repair:** use a per-entity presentation timeline and continuous bounded position/velocity correction; reserve snapping for discontinuities. Stop or reconcile local movement after the prediction horizon. Use one presentation time for a horse and its rider. Simply raising packet frequency cannot fix this branch logic.

### 3. NPC mounting records requested state as completed state — high priority

**Function:** `MaintainWorldMirrorGuest` (14674–14687, 14744–14758).

`mountedRelationReady` means the desired parent's handle exists, not that the rider is attached. The function calls `SetPedOntoMount` and immediately sets `entry.mounted = true`, without confirming completion or throttling retries. A failed mount can consequently be reissued every maintenance tick, while independent rider motion is suppressed.

On dismount, it calls `TaskDismountAnimal` once and immediately sets `entry.mounted = false`. If that task is delayed, interrupted, or rejected, later frames no longer enter the retry branch. Movement tasks issued later in the same function may also compete with the in-progress transition. A missing parent is treated as a dismount even if the authoritative state still requests mounting.

**Repair:** track desired parent, actual attachment, and pending transition separately. Confirm the native result, throttle retries, and suppress conflicting tasks until transition completion. Treat a temporarily unavailable parent as pending. The existing remote-player `reconcileRider` lambda in `MaintainRemoteMount` (approximately 13458–13505) already checks actual native state and retries both directions; extract a shared policy for NPCs rather than maintaining divergent implementations.

### 4. Navigation deduplication ignores speed changes and task completion — medium priority

**Function:** `MaintainWorldMirrorGuest` (14922–15026).

For ordinary locomotion/fleeing, refresh occurs on a task-kind change or at least 2 m of destination change. Time-based refresh only applies to scenarios and cinematics. Speed is supplied to `TASK_FOLLOW_NAV_MESH_TO_COORD` only when the task starts. Task completion/interruption and speed changes are not checked. Recreating a vanished proxy also leaves `previousTaskMs`, `previousTaskKind`, and `previousTaskTarget` intact (14353–14368).

**Consequence:** a stopped/interrupted proxy can remain idle while the cached task signature suppresses restarting it. A speed change that leaves the destination within the threshold does not update the native task. A recreated proxy can inherit bookkeeping for a task that only existed on the deleted ped.

**Repair:** include movement speed/mode and actual task status in the refresh policy, use bounded retries, and reset task caches when the local handle is replaced.

### 5. NPC animation data is insufficient for matching action reactions — architectural gap

**Functions/data:** `SampleWorldEntities` (5900–5974), `MaintainWorldMirrorGuest` (14849–15066), and `WorldEntityStatePayload` in `FrameCodec.hpp` (532).

The NPC payload contains health, velocity, broad task kind, and aiming/firing flags. It has no action-event ID, hit reaction, ragdoll/get-up state, animation phase, or mount-transition identity. The guest deliberately ignores firing as a source of local bullets, but there is also no corresponding cosmetic firing-event replay in this path. A short event between the normal 100 ms samples can be missed entirely.

**Consequence:** matching transforms and health still cannot guarantee matching shot, hit, rearing, fall, or recovery animations. This is a missing capability, not evidence that one specific animation native is faulty.

**Repair:** add bounded, sequenced action events with entity generation and cancellation/priority rules. Keep cosmetic reactions separate from authoritative damage. The pre-existing `RemoteAnimationBuffer` changes use `PlayerAnimationStatePayload`; they do not automatically cover these world NPCs.

## Concrete WitcherOnline comparison

Reference file: [remotePlayer.ws at the reviewed commit](https://github.com/rejuvenate7/WitcherOnline/blob/4755ee753dc66a780ae4a72ce2b75f4fecf5c0f4/witcher/mods/modWitcherOnline/content/scripts/local/remotePlayer.ws).

| Witcher function | Relevant behavior | RDR2 implication |
|---|---|---|
| `moveEntity` (5867), `moveHorse` (6255) | Uses movement-adjustor `SlideTo` and rotation requests; horse gait is selected separately. Rider movement is skipped while mounted/riding. | Separate trajectory following from animation and attachment ownership; RDR2 needs its own native implementation. |
| `queueAnim` (397), `updateAnimState` (551) | Rejects repeated current/queued animations, bounds the queue, and protects selected animations from locomotion replacement. | Deduplicate actual actions while allowing locomotion targets to remain current. |
| `updateGeraltAnims` (2879; action check at 5699) | Consumes exploration actions when `lastActionTime` changes. | NPC actions need event identity, rather than replaying each sampled flag. |
| `spawnHorse` (6127) | Checks `IsUsingHorse(true)` and limits mount requests to one per 0.5 seconds. | NPC mounting needs completion checks and retry throttling. |
| `checkRidingAttachment` (1306) | Checks attachment presence and transitions between horse, boat, and player attachment types. | Maintain explicit relationship state and transition cleanup. |

The accompanying [client.ws](https://github.com/rejuvenate7/WitcherOnline/blob/4755ee753dc66a780ae4a72ce2b75f4fecf5c0f4/witcher/mods/modWitcherOnline/content/scripts/local/client.ws) implements `attachRiderHorse` (2479), `detachRiderSafe` (5262), and movement-sequence rejection in `updatePlayerMovement` (4028). Bone names, offsets, and Witcher engine APIs are not portable to RDR2.

## What already exists in RDR2

`WorldMirrorGuestGraph::ApplyState` rejects duplicate/stale sequences (`WorldMirror.cpp:698`). `NetworkBridgeDeliveryPump` coalesces pending entity updates per entity and prefers newer sequences (`NetworkBridgeDeliveryPump.cs:186`). Parent-first ordering and pending-parent handling also exist in the world graph. Do not diagnose their absence solely from the screenshot or replace these mechanisms before measuring them.

## Validation required before claiming the clip is fixed

1. Record synchronized host/guest views with build ID, entity ID, task kind, desired/current position, error, snapshot age, and native task status.
2. Walk/strafe a combat NPC with reliable delivery: verify motion continues while aiming and error stays bounded below the snap threshold.
3. Run horses through turns and speed changes; then inject packet delay/loss and verify bounded prediction followed by a controlled stop.
4. Delay parent creation, interrupt mounting/dismounting, and recreate a proxy handle: verify retries converge without frame-by-frame task restarts or stale task caches.
5. Replay duplicated/reordered action events and verify each reaction plays once, without introducing duplicate damage.

This was a static code review plus visual clip inspection, not a two-client runtime reproduction. Existing SDK-free world-graph tests exercise ordering/lifecycle, but do not validate native task and animation behavior inside `MaintainWorldMirrorGuest`. No runtime fix or test pass is claimed.
