# NPC action replication

## Status

The host-to-guest action transport and the SDK-observable native playback path are implemented. **This is not full clip-for-clip NPC animation replication.** The SDK path has no generic way to obtain the currently playing host clip, its phase, blend weights, or paired actor roles. Melee, precise hit reactions, horse rearing, and mounted firing overlays remain gaps. Compiling native calls does not establish their visual result in RDR2.

Protocol **34** adds reliable message `NpcAnimation = 50`. Both players need matching protocol-34 sidecars and bridges. The bridge build identifier now includes `p34`.

## Implemented behavior

- The host samples admitted NPCs every bridge tick, independently of the 100 ms world-state sampling interval.
- Clip-ammo decreases capture shots, including consecutive shots while the shooting native remains true. A shooting rising edge is the fallback. Health decreases capture hit events. Baselines and reconnect baselines suppress old transient events.
- Reload, ragdoll, recovery, melee, mounted, dead, jump and climb flags form a complete current state. Transitions send immediately; 500 ms heartbeats repair missing sustained state.
- Each stable world ID has a monotonically advancing revision and a bounded 16-entry journal. Temporary pipe pressure retains unsent entries. Overflow drops the oldest entry; reconnect clears pending pulses while retaining revision continuity for retained IDs.
- Capture retries, the guest sidecar delivery queue, and the native inbox spend the lease instead of renewing it. The initial lease is 1,000 ms; wire leases below 100 ms are discarded. This bounds local queue delay; it is not a synchronized measurement of network transit time.
- The reliable control lane preserves FIFO order, and NPC events are excluded from snapshot coalescing. The guest rejects duplicate/stale revisions. The SDK only accepts registered ped IDs with the expected model; unloaded registered models can queue events until expiry. Missing graph parents do not allocate orphan animation inboxes.
- Death, scenario ownership, despawn, proxy recreation and session cleanup close pending playback. Physical recovery has a bounded grace period after the host lease ends. Ordinary movement, root correction and aiming yield while a supported action owns the ped.
- Guest damage remains host-authoritative. Cosmetic shooting requests empty total and clip ammunition, then cancel the task before restoring ammunition. No local damaging bullet or autonomous melee task is introduced.

## Playback coverage

| Host observation | Guest behavior | Limit |
| --- | --- | --- |
| Unmounted reload | Equip the weapon, empty clip, request native reload; retry if absent; release on end/expiry | Native timing; no host clip phase |
| Unmounted stationary shot | Gunshot audio and a bounded empty-ammo firing-graph task | Recoil depends on the native graph; visual result unverified |
| Moving/mounted shot | Gunshot audio; preserve current movement/mount task | Recoil overlay is not implemented |
| Hit | Pain audio; health remains in world state | Exact hit animation is not implemented; logs identify the fallback |
| Ragdoll | Bounded native ragdoll with sampled velocity; refreshed while host state persists | Local physics will not reproduce identical body pose |
| Getting up | Stop refreshing ragdoll; allow native recovery without movement interference | Exact recovery clip/phase is not synchronized |
| Jump/climb | Edge-triggered native jump/climb; protect task while leased | Depends on local terrain and native task acceptance |
| Melee | Replicate observed state and log missing clip support | Attack/block/grapple clips and paired interactions are not implemented |
| Mounted state | Existing stable parent relation and confirmed mount/dismount policy | No new horse rearing or wagon-seat animation support |
| Death | Latch terminal state and let world health/corpse logic apply | No exact death pose replication |
| Scenario/cinematic | Existing scenario/AnimScene owner retains control | Generic action capture/playback is bypassed |

Discrete pulses drain on separate guest game ticks so a reliable burst does not overwrite several native tasks in one tick. Sustained states do not restart native tasks on every world snapshot.

## Code map

- `src/CoopStory.Bridge/include/coopstory/bridge/NpcAnimation.hpp` and `src/CoopStory.Bridge/src/NpcAnimation.cpp`: wire codec, capture journal, revision fence, expiry and ownership priority.
- `src/CoopStory.Protocol/NpcAnimationCodec.cs`: matching managed codec and lease aging.
- `BridgeRuntime::TickNpcAnimations`: host capture and retry; inbound `NpcAnimation` handling gates guest dispatch by active world ID.
- `ScriptHookSdkFacade::SampleNpcAnimation`, `QueueNpcAnimation`, `MaintainNpcAnimation`: native observation, admission and playback.
- `ScriptHookSdkFacade::MaintainWorldMirrorGuest`: action ownership, movement suppression, death and mount integration.
- `SidecarRuntime`: host-only authorization, strict validation and diagnostics. `NetworkBridgeDeliveryPump`: ordered delivery with queue lease aging.

The protocol intentionally carries observations and events, not arbitrary native names, pointers or unverified clip hashes received from a peer.

## Validation

Automated coverage includes a shared C++/C# golden wire fixture, malformed payload rejection, host authority in both directions, reliable FIFO delivery, tick-rate capture, consecutive shots, reload suppression of false shots, send retry, lease boundaries, journal overflow, revision wrap, duplicate rejection, reconnect baselining, handle-recreation revision retention, and runtime spawn/despawn admission.

Commands:

```powershell
cmake --build --preset bridge-asi-vs2026-release
ctest --preset bridge-asi-vs2026-release
dotnet build CoopStory.slnx -c Release --no-restore
dotnet run --project tests/CoopStory.SelfTest -c Release --no-restore
```

Final validation: the SDK-enabled ASI build succeeded; **77/77 bridge tests** and **53/53 managed tests** passed, including the Guest World View action round trip. The full managed solution builds with zero warnings and zero errors. Automated tests cannot validate actual animation appearance, terrain-dependent task success, audio duplication, or cross-machine physics. No two-client game validation or installation into the game directory has been performed.

For a two-client acceptance run, compare simultaneous host/guest recordings of stationary fire, moving fire, reload interruption, knockdown/recovery, horse dismount during knockdown, jump/climb, death during reload, scenario entry, model-loading delay, and reconnect under latency. Inspect `[NPC_ANIMATION]` fallback logs and protocol/build IDs. The solo Guest World View mode also echoes actions with remapped entity IDs and target coordinates; a pipe/network integration test verifies this path.

Full animation fidelity requires an additional verified capture/playback integration for clip identity, phase, blends and paired actors, plus suitable mounted/animal animation bindings. WitcherOnline's player queue/deduplication patterns do not supply those RDR2 bindings.
